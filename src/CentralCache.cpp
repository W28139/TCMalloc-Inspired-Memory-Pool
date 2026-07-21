/**
 * @file    CentralCache.cpp
 * @brief   中心缓存层 —— 全局共享的内存调度中枢
 *
 * ── CentralCache 在三层架构中的位置 ──
 *
 *   [ThreadCache]   ← 线程私有，无锁（针对用户，用户每个线程可对应一个ThreadCache线程
 *        ↕
 *   [CentralCache]  ← 全局共享，桶级自旋锁（针对CentralCache多个线程争夺获取同大小块内存的
 *        ↕
 *   [PageCache]     ← 全局共享，互斥锁，直接与 OS 交互
 *
 * ── 核心职责 ──
 *
 *   1. 向 ThreadCache 批量分发内存块（fetchRange）
 *   2. 接收 ThreadCache 归还的多余内存块（returnRange）
 *   3. 内存不足时，从 PageCache 申请新的 Span，切分成小块
 *   4. 跟踪每个 Span 的空闲块数量，整 Span 空闲时归还 PageCache
 *
 * ── 数据结构概览 ──
 *
 *   centralFreeList_[index]  → 该大小类别的空闲块链表头
 *   locks_[index]            → 该大小类别的自旋锁（桶锁）
 *   spanTrackers_[0..1023]   → Span 追踪器数组，记录每个 Span 的空闲情况
 *   delayCounts_[index]      → 该大小类别的延迟归还计数器
 */

#include "../include/CentralCache.h"
#include "../include/PageCache.h"
#include <cassert>
#include <thread>
#include <chrono>
#include <algorithm>

namespace Kama_memoryPool
{

// =========================================================================
// 静态常量定义
// =========================================================================

// 延迟归还的时间间隔：1 秒
// 含义：一个大小类别至少隔 1 秒才会触发一次"检查是否有整 Span 可以归还"的操作
// 目的：防止频繁的 Span 归还和重新申请造成的"抖动"
const std::chrono::milliseconds CentralCache::DELAY_INTERVAL{1000};

// 每次向 PageCache 申请 Span 时，默认申请 8 页（8 × 4096B = 32KB）
// 对于大多数小对象请求（≤32KB 的大小类别），统一申请 8 页
// 好处：减少 PageCache 调用频率，避免 PageCache 产生过多碎片
static const size_t SPAN_PAGES = 8;

// =========================================================================
// 构造函数
// =========================================================================

CentralCache::CentralCache()
{
    // --- 初始化所有自由链表头为 nullptr ---
    // centralFreeList_ 是一个 std::array<std::atomic<void*>, 32768>
    // 每个元素对应一个大小类别（8B, 16B, 24B, ..., 256KB）
    // 初始时所有链表都是空的
    for (auto& ptr : centralFreeList_)
    {
        ptr.store(nullptr, std::memory_order_relaxed);
    }

    // --- 初始化所有自旋锁为"未锁定"状态 ---
    // locks_[i] 保护 centralFreeList_[i] 的并发访问
    // clear() 将 atomic_flag 设为 false（未锁定）
    for (auto& lock : locks_)
    {
        lock.clear();
    }

    // --- 初始化延迟归还相关 ---
    // delayCounts_[i]：记录大小类别 i 累计接收了多少次归还
    // 每调用一次 returnRange 就 +1，达到 MAX_DELAY_COUNT(48) 或超过 1 秒后触发检查
    for (auto& count : delayCounts_)
    {
        count.store(0, std::memory_order_relaxed);
    }

    // lastReturnTimes_[i]：记录大小类别 i 上一次执行延迟归还的时间
    // 初始化为"现在"，防止刚启动就触发归还检查
    for (auto& time : lastReturnTimes_)
    {
        time = std::chrono::steady_clock::now();
    }

    // spanCount_：已使用的 SpanTracker 数量（同时也是下一个可用槽位的下标）
    spanCount_.store(0, std::memory_order_relaxed);

    // --- 初始化 SpanTracker 动态数组（无锁读结构）---
    // 预分配 TRACKER_INITIAL_CAPACITY 个 SpanTracker，构建扁平指针数组
    SpanTracker** initialArray = new SpanTracker*[TRACKER_INITIAL_CAPACITY];
    for (size_t i = 0; i < TRACKER_INITIAL_CAPACITY; ++i)
    {
        trackerStorage_.emplace_back();
        initialArray[i] = &trackerStorage_.back();
    }
    trackerArray_.store(initialArray, std::memory_order_release);
    trackerCount_.store(TRACKER_INITIAL_CAPACITY, std::memory_order_release);
}

// =========================================================================
// fetchRange —— 向 ThreadCache 批量分发内存块
// =========================================================================
//
// 这是 CentralCache 最核心的"出货"函数。ThreadCache 在本地缓存不足时调用它。
//
// 参数：
//   index - 大小类别索引。例如 index=2 对应 (2+1)*8=24→向上取整到32字节的大小类
//
// 返回值：
//   一段链表的头指针（void*），链表包含最多 BATCH_SIZE(8) 个内存块。
//   ThreadCache 取走第一块，其余 BATCH_SIZE-1 块进入其本地 freeList_，
//   后续 allocate 直接命中 ThreadCache，无需再跨层调用。
//
// 流程概要：
//   ┌─ 检查 index 合法性
//   ├─ 获取自旋锁 locks_[index]
//   ├─ 尝试从 centralFreeList_[index] 取已有的空闲块
//   │   ├─ 有 → 批量取 BATCH_SIZE 块，断开链表，逐个更新 SpanTracker
//   │   └─ 无 → 从 PageCache 申请新 Span → 切分 → 批量取 BATCH_SIZE 块
//   └─ 释放自旋锁，返回批次链表

void* CentralCache::fetchRange(size_t index)
{
    // ---- 第 0 步：索引合法性检查 ----
    // index 的范围是 [0, FREE_LIST_SIZE-1]，即 [0, 32767]
    // 如果越界，说明申请的内存超过了 MAX_BYTES(256KB)，不应由内存池处理
    if (index >= FREE_LIST_SIZE)
        return nullptr;

    // ---- 第 1 步：获取自旋锁 ----
    // test_and_set：原子地将标志位设为 true，并返回旧值
    //   - 返回 false → 之前没人持锁 → 你获取到了锁，退出循环
    //   - 返回 true  → 有人在用 → 循环等待，每次 yield() 让出 CPU
    //
    // memory_order_acquire：确保获取锁之后，对共享变量的读取能看到上一个持锁者（用 release 释放锁）的所有写入
    while (locks_[index].test_and_set(std::memory_order_acquire))
    {
        std::this_thread::yield(); // 让出 CPU 时间片，不空转浪费 CPU
    }

    void* result = nullptr;

    try
    {
        // ---- 第 2 步：检查中心缓存是否有现成的空闲块 ----
        // 使用 relaxed 读取：不要求精确的跨线程同步，因为我们已经持有了锁
        result = centralFreeList_[index].load(std::memory_order_relaxed);

        if (!result)
        {
            // ================================================
            // 分支 A：中心缓存为空 → 需要从 PageCache 申请新 Span
            // ================================================

            // 根据 index 反算该大小类别的实际块大小
            // 例：index=2 → (2+1)*8 = 24 → 这个大小类别服务于 17~24 字节的请求
            size_t size = (index + 1) * ALIGNMENT;

            // 调用 fetchFromPageCache → PageCache::allocateSpan → 可能触发 mmap
            // 返回的 result 是一块连续内存的起始地址（Span 的起始地址）
            result = fetchFromPageCache(size);

            // 如果 PageCache 也分配失败了（系统内存耗尽），释放锁并返回 nullptr
            if (!result)
            {
                locks_[index].clear(std::memory_order_release);
                return nullptr;
            }

            // --- 将 Span 这块连续内存切分成多个 size 大小的小块 ---
            char* start = static_cast<char*>(result);

            // 计算实际分配的页数（用于计算 blockNum）
            // 逻辑：
            //   - 如果该大小类 ≤ 32KB(8页) → 实际上申请了固定 8 页
            //   - 如果该大小类 > 32KB        → 按实际需求向上取整
            size_t numPages = (size <= SPAN_PAGES * PageCache::PAGE_SIZE) ?
                                     SPAN_PAGES :
                                     (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

            // 计算这个 Span 可以切分成多少个 size 大小的块
            // 例：8页 × 4096 / 32字节 = 32768 / 32 = 1024 块
            size_t blockNum = (numPages * PageCache::PAGE_SIZE) / size;

            // 至少要有 2 块才值得构建链表（1 块就直接返回不用链了）
            if (blockNum > 1)
            {
                // --- 构建自由链表：将切分的小块串成单向链表 ---
                //
                // 内存布局（假设 size=32）：
                //   start →
                //   [块0: 0x1000] [块1: 0x1020] [块2: 0x1040] ... [块N: ...]
                //
                // 构建后链表的逻辑结构：
                //   块0 → 块1 → 块2 → ... → 块(N-1) → nullptr
                //
                // 注意：链表指针（next）存储在每块的前 8 字节内
                for (size_t i = 1; i < blockNum; ++i)
                {
                    // current = 第 i-1 块（从 0 开始）的起始地址
                    void* current = start + (i - 1) * size;
                    // next    = 第 i 块的起始地址
                    void* next    = start + i * size;

                    // 在 current 块的前 8 字节写入 next 的地址
                    // 即：第 i-1 块.next = 第 i 块
                    *reinterpret_cast<void**>(current) = next;
                }

                // 最后一块的前 8 字节写入 nullptr，标记链表结束
                *reinterpret_cast<void**>(start + (blockNum - 1) * size) = nullptr;

                // --- 批量传输：从链表头取 BATCH_SIZE 块给 ThreadCache ---
                //
                // 此时链表：result(块0) → 块1 → ... → 块(BATCH_SIZE-1) → ... → nullptr
                // 目标：前 BATCH_SIZE 块作为一批返回，其余留在 CentralCache
                //
                // 找出第 BATCH_SIZE 块（批次的尾节点）
                size_t batchCount = 1;
                void* batchTail = result;
                while (batchCount < BATCH_SIZE && batchCount < blockNum &&
                       *reinterpret_cast<void**>(batchTail) != nullptr)
                {
                    batchTail = *reinterpret_cast<void**>(batchTail);
                    batchCount++;
                }

                // 保存批次之后剩余链表的头
                void* rest = *reinterpret_cast<void**>(batchTail);

                // 在批次尾断开连接
                *reinterpret_cast<void**>(batchTail) = nullptr;

                // 剩余链表挂回 CentralCache
                centralFreeList_[index].store(rest, std::memory_order_release);

                // --- 记录 SpanTracker ---
                size_t trackerIndex = spanCount_++;

                if (trackerIndex >= trackerCount_.load(std::memory_order_relaxed))
                {
                    expandTrackerArray(trackerIndex);
                }

                SpanTracker** array = trackerArray_.load(std::memory_order_acquire);
                SpanTracker* tracker = array[trackerIndex];

                tracker->spanAddr.store(start, std::memory_order_release);
                tracker->numPages.store(numPages, std::memory_order_release);
                tracker->blockCount.store(blockNum, std::memory_order_release);
                // freeCount：初始空闲 = 总块数 - 已取走的批次块数
                tracker->freeCount.store(blockNum - batchCount, std::memory_order_release);

                // 未排序条目超阈值时触发重排序
                size_t total = spanCount_.load(std::memory_order_relaxed);
                size_t sorted = sortedCount_.load(std::memory_order_relaxed);
                if (total > sorted && (total - sorted) > SORT_THRESHOLD)
                {
                    ensureSorted();
                }
            }
            else
            {
                // blockNum == 1：大对象（>16KB）场景，只有一块无需构建链表，
                // 但仍需 SpanTracker，否则该 Span 永远无法归还 PageCache
                size_t trackerIndex = spanCount_++;

                if (trackerIndex >= trackerCount_.load(std::memory_order_relaxed))
                {
                    expandTrackerArray(trackerIndex);
                }

                SpanTracker** array = trackerArray_.load(std::memory_order_acquire);
                SpanTracker* tracker = array[trackerIndex];

                tracker->spanAddr.store(start, std::memory_order_release);
                tracker->numPages.store(numPages, std::memory_order_release);
                tracker->blockCount.store(1, std::memory_order_release);
                // freeCount = 0：唯一一块已返回给 ThreadCache，Span 中无空闲块
                tracker->freeCount.store(0, std::memory_order_release);

                // 检查是否需要重排序
                size_t total = spanCount_.load(std::memory_order_relaxed);
                size_t sorted = sortedCount_.load(std::memory_order_relaxed);
                if (total > sorted && (total - sorted) > SORT_THRESHOLD)
                {
                    ensureSorted();
                }
            }
        }
        else
        {
            // ================================================
            // 分支 B：中心缓存有现成的空闲块 → 批量取 BATCH_SIZE 块
            // ================================================

            // 遍历找出批次的尾节点（第 BATCH_SIZE 块，或链表末尾）
            size_t batchCount = 1;
            void* batchTail = result;
            while (batchCount < BATCH_SIZE &&
                   *reinterpret_cast<void**>(batchTail) != nullptr)
            {
                batchTail = *reinterpret_cast<void**>(batchTail);
                batchCount++;
            }

            // 保存批次之后剩余链表的头
            void* rest = *reinterpret_cast<void**>(batchTail);

            // 在批次尾断开连接
            *reinterpret_cast<void**>(batchTail) = nullptr;

            // 剩余链表挂回 CentralCache
            centralFreeList_[index].store(rest, std::memory_order_release);

            // --- 更新 SpanTracker：批次中每块的 freeCount 减 1 ---
            // 批次内块可能来自不同 Span，逐块查找并递减
            void* current = result;
            for (size_t i = 0; i < batchCount; ++i)
            {
                SpanTracker* tracker = getSpanTracker(current);
                if (tracker)
                {
                    tracker->freeCount.fetch_sub(1, std::memory_order_release);
                }
                current = *reinterpret_cast<void**>(current);
            }
        }
    }
    catch (...)
    {
        // --- 异常安全：无论发生什么，都要释放锁 ---
        // 如果持锁期间抛异常（如 PageCache 内部出错），
        // 必须释放锁，否则其他线程将永远阻塞在这个 index 的锁上
        locks_[index].clear(std::memory_order_release);
        throw; // 重新抛出，让上层（ThreadCache）处理
    }

    // ---- 第 3 步：释放自旋锁 ----
    // memory_order_release：确保之前的所有写入（链表更新、SpanTracker 更新）
    // 对下一个获取锁的线程（用 acquire）可见
    locks_[index].clear(std::memory_order_release);

    // 返回取到的内存块（void* 指针，8 字节对齐）
    return result;
}

// =========================================================================
// returnRange —— 接收 ThreadCache 归还的多余内存块
// =========================================================================
//
// ThreadCache 发现某个大小类别缓存了太多空闲块（>256 个），
// 会把超出部分（保留 1/4，归还 3/4）串成链表，调用本函数归还。
//
// 参数：
//   start - 归还链表的头节点地址
//   size  - 归还的总字节数（块数 × 每块字节数）
//   index - 大小类别索引
//
// 流程概要：
//   ┌─ 基础检查
//   ├─ 获取自旋锁
//   ├─ 找到归还链表的尾节点
//   ├─ 头插法：将 CentralCache 现有链表接在归还链表后面
//   ├─ 延迟计数 +1
//   └─ 判断是否触发延迟归还检查

void CentralCache::returnRange(void* start, size_t size, size_t index)
{
    // ---- 第 0 步：基础检查 ----
    // start 为空：没有东西要归还
    // index 越界：不是内存池管理的大小范围
    if (!start || index >= FREE_LIST_SIZE)
        return;

    // 计算该大小类别下每块的字节数
    // 例：index=2 → blockSize = 3*8 = 24 字节
    size_t blockSize = (index + 1) * ALIGNMENT;

    // 计算一共归还了多少块
    // 例：size=720 字节，blockSize=24 → blockCount=30 块
    size_t blockCount = size / blockSize;

    // ---- 第 1 步：获取自旋锁 ----
    while (locks_[index].test_and_set(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }

    try
    {
        // ---- 第 2 步：找到归还链表的尾节点 ----
        //
        // 归还链表的结构：
        //   start → 块A → 块B → ... → end(尾节点) → ???
        //
        // 我们需要找到 end，然后把 CentralCache 的现有链表接到 end 后面
        //
        // 遍历方法：
        //   从 start 出发，反复读每块的前 8 字节（next 指针），
        //   直到 next == nullptr（链表末尾）或遍历够了 blockCount 块
        void* end = start;
        size_t count = 1; // 已经算上 start 本身
        while (*reinterpret_cast<void**>(end) != nullptr && count < blockCount)
        {
            end = *reinterpret_cast<void**>(end); // 移动到下一块
            count++;
        }

        // ---- 第 3 步：将 CentralCache 现有链表接到归还链表后面 ----
        //
        // 合并前：
        //   归还链表:   start → ... → end → nullptr
        //   中心缓存:   oldHead → ... → nullptr
        //
        // 合并后：
        //   centralFreeList_[index]:
        //   start → ... → end → oldHead → ... → nullptr
        //
        // 这是"头插法"的批量版本：整个归还链表作为一个整体，
        // 插在 CentralCache 现有链表的前面

        // 读取 CentralCache 当前的链表头
        void* current = centralFreeList_[index].load(std::memory_order_relaxed);

        // 把 CentralCache 的链表头接到归还链表的尾部
        *reinterpret_cast<void**>(end) = current;

        // 把归还链表的头部设为 CentralCache 的新链表头
        centralFreeList_[index].store(start, std::memory_order_release);

        // ---- 第 4 步：延迟归还计数 ----
        //
        // 每次归还并不立即检查"Span 是否完全空闲"，而是累加计数
        // fetch_add(1) 返回旧值，+1 得到本次调用后的新值
        size_t currentCount = delayCounts_[index].fetch_add(1, std::memory_order_relaxed) + 1;
        auto currentTime = std::chrono::steady_clock::now();

        // ---- 第 5 步：判断是否触发延迟归还检查 ----
        //
        // 两个条件满足其一就触发：
        //   条件 A：累计归还次数 ≥ MAX_DELAY_COUNT（48 次）
        //   条件 B：距上次检查已经过去了 DELAY_INTERVAL（1 秒）
        if (shouldPerformDelayedReturn(index, currentCount, currentTime))
        {
            // 遍历 centralFreeList_，按 Span 归组统计空闲块
            // 如果有 Span 的所有块都空闲 → 归还给 PageCache
            performDelayedReturn(index);
        }
    }
    catch (...)
    {
        locks_[index].clear(std::memory_order_release);
        throw;
    }

    // ---- 第 6 步：释放自旋锁 ----
    locks_[index].clear(std::memory_order_release);
}

// =========================================================================
// shouldPerformDelayedReturn —— 判断是否应该触发延迟归还检查
// =========================================================================
//
// 设计理念：
//   归还操作很频繁（每次 ThreadCache 超阈值都会调用 returnRange），
//   如果每次归还都去遍历链表、统计 Span 空闲情况，代价太高。
//   所以设置两个宽松的触发条件，批量处理。
//
// 两个条件：
//   1. 累积次数 ≥ 48：归还太频繁了，值得花时间检查一下
//   2. 距上次检查 ≥ 1 秒：即便频率低，定时也需要检查，防止 Span 长期空闲

bool CentralCache::shouldPerformDelayedReturn(size_t index, size_t currentCount,
    std::chrono::steady_clock::time_point currentTime)
{
    // 条件 1：计数触发 —— 累计归还操作达到 48 次
    if (currentCount >= MAX_DELAY_COUNT)
    {
        return true;
    }

    // 条件 2：时间触发 —— 距上次检查已经过了 1 秒
    auto lastTime = lastReturnTimes_[index];
    return (currentTime - lastTime) >= DELAY_INTERVAL;
}

// =========================================================================
// performDelayedReturn —— 执行延迟归还，找出完全空闲的 Span 还给 PageCache
// =========================================================================
//
// 这个函数是"归还 Span"的真正执行者。它遍历某个大小类别的整个空闲链表，
// 统计每个 Span 中有多少空闲块，然后逐个检查是否满足归还条件。
//
// 流程：
//   ┌─ 重置计数器和时钟
//   ├─ 遍历 centralFreeList_[index]，按 Span 分组统计空闲块数
//   └─ 对每个 Span，调用 updateSpanFreeCount 检查是否可归还

void CentralCache::performDelayedReturn(size_t index)
{
    // --- 重置延迟归还状态 ---
    delayCounts_[index].store(0, std::memory_order_relaxed);
    lastReturnTimes_[index] = std::chrono::steady_clock::now();

    // 两遍扫描替代 unordered_map，消除自旋锁临界区内的堆分配：
    //   第一遍：遍历 centralFreeList_，在 SpanTracker.scanCount 上原子累加
    //   第二遍：遍历所有 SpanTracker，取出 scanCount 并调用 updateSpanFreeCount

    // --- 第一遍：按 Span 累加空闲块计数 ---
    void* currentBlock = centralFreeList_[index].load(std::memory_order_relaxed);
    while (currentBlock)
    {
        SpanTracker* tracker = getSpanTracker(currentBlock);
        if (tracker)
        {
            tracker->scanCount.fetch_add(1, std::memory_order_relaxed);
        }
        currentBlock = *reinterpret_cast<void**>(currentBlock);
    }

    // --- 第二遍：处理有 scanCount > 0 的 Span ---
    SpanTracker** array = trackerArray_.load(std::memory_order_acquire);
    size_t total = spanCount_.load(std::memory_order_relaxed);

    for (size_t i = 0; i < total; ++i)
    {
        SpanTracker* tracker = array[i];
        // 原子交换取出计数并清零，为下次扫描做准备
        size_t count = tracker->scanCount.exchange(0, std::memory_order_relaxed);
        if (count > 0)
        {
            updateSpanFreeCount(tracker, count, index);
        }
    }
}

// =========================================================================
// updateSpanFreeCount —— 更新 Span 的空闲计数，并在全空闲时归还 PageCache
// =========================================================================
//
// 这是归还整 Span 的"最终裁决"函数。
//
// 逻辑：
//   1. 将新统计的空闲块数加到 SpanTracker.freeCount 上
//   2. 如果 freeCount == blockCount（所有块都空闲了）
//      → 从 centralFreeList_ 中摘除属于该 Span 的所有块
//      → 调用 PageCache::deallocateSpan 归还整块连续内存
//
// 参数：
//   tracker       - 指向该 Span 的 SpanTracker
//   newFreeBlocks - 本次统计发现该 Span 在空闲链表中的块数
//   index         - 大小类别索引（用于访问 centralFreeList_）

void CentralCache::updateSpanFreeCount(SpanTracker* tracker, size_t freeBlocksInList, size_t index)
{
    // --- 第 1 步：更新空闲计数（直接设置，而非累加）---
    //
    // 为什么用直接设置而不是累加？
    //   performDelayedReturn 每次扫描的是 centralFreeList_ 中【当前所有】的空闲块，
    //   这里面包含了前几次扫描就已经在链表中的块（如果它们没被 fetchRange 取走）。
    //   如果用累加（oldFreeCount + freeBlocksInList），同一批块会被反复计数，
    //   导致 freeCount 虚高、超过 blockCount，永远触发不了 (freeCount == blockCount) 的归还条件。
    //
    //   直接设置 freeCount = freeBlocksInList 的语义：
    //     freeCount 表示"此刻在 centralFreeList_ 中该 Span 的空闲块数"。
    //     fetchRange 取走块时已经做了 fetch_sub(-1)，扫描时用实际链表计数覆盖，
    //     可以纠正 returnRange 带来的增量（returnRange 不更新 freeCount）。
    //
    //   关于 ThreadCache 持有块的场景：
    //     如果 ThreadCache 本地 freeList_ 还缓存着该 Span 的块，它们不在 centralFreeList_ 中，
    //     本次扫描统计不到，freeCount < blockCount，Span 不会被归还。
    //     但等 ThreadCache 归还后，下一次 performDelayedReturn 扫描就能统计到全部块，
    //     Span 最终会被正确归还——只是时机延后，不是"永不归还"。
    tracker->freeCount.store(freeBlocksInList, std::memory_order_release);

    // --- 第 2 步：判断是否全部空闲 ---
    // 全部空闲的判定：freeCount == blockCount
    //   blockCount = 该 Span 总共被切分成的块数
    //   freeCount  = 当前在 centralFreeList_ 中该 Span 的空闲块数
    if (freeBlocksInList == tracker->blockCount.load(std::memory_order_relaxed))
    {
        // 读取 Span 的元信息
        void* spanAddr = tracker->spanAddr.load(std::memory_order_relaxed);
        size_t numPages = tracker->numPages.load(std::memory_order_relaxed);

        // --- 第 3 步：从 centralFreeList_ 中摘除属于该 Span 的所有块 ---
        //
        // 为什么需要"摘除"而不是"直接归还"？
        //   centralFreeList_ 中有指向这些块的指针，如果直接归还 Span 给 PageCache，
        //   后续 fetchRange 可能通过链表访问到已释放的内存 → 悬挂指针 (Dangling Pointer)
        //
        // 摘除策略：遍历链表，按地址范围过滤
        //   属于该 Span 的块 → 跳过（不放进新链表）
        //   不属于该 Span 的块 → 保留（串入新链表）
        //
        // 判断方法：块地址是否在 [spanAddr, spanAddr + numPages * PAGE_SIZE) 范围内
        void* head = centralFreeList_[index].load(std::memory_order_relaxed);
        void* newHead = nullptr; // 摘除后的新链表头
        void* prev = nullptr;    // 遍历时的前驱节点
        void* current = head;    // 遍历时的当前节点

        while (current)
        {
            // 取出下一个节点（在修改链表前保存）
            void* next = *reinterpret_cast<void**>(current);

            // 判断 current 是否属于要归还的 Span 的地址范围内
            if (current >= spanAddr &&
                current < static_cast<char*>(spanAddr) + numPages * PageCache::PAGE_SIZE)
            {
                // 这个块属于要归还的 Span → 从链表中移除
                if (prev)
                {
                    // 有前驱 → 让前驱跳过当前节点
                    *reinterpret_cast<void**>(prev) = next;
                }
                else
                {
                    // 没有前驱 → 当前是链表头 → 新链表头从 next 开始
                    newHead = next;
                }
                // 注意：prev 不变（当前节点被跳过，前驱仍然是 prev）
            }
            else
            {
                // 这个块不属于要归还的 Span → 保留在新链表中
                prev = current; // 更新前驱为当前节点
            }
            current = next; // 继续遍历
        }

        // 更新 centralFreeList_，替换为摘除后的新链表
        centralFreeList_[index].store(newHead, std::memory_order_release);

        // --- 第 4 步：归还 Span 给 PageCache ---
        // PageCache 收到后：
        //   1. 查找相邻的空闲 Span，尝试合并（对抗外部碎片）
        //   2. 将 Span（可能是合并后的）插入 freeSpans_ 等待复用
        PageCache::getInstance().deallocateSpan(spanAddr, numPages);
    }
}

// =========================================================================
// fetchFromPageCache —— 向 PageCache 申请新 Span
// =========================================================================
//
// 根据请求的内存大小，决定申请多少页：
//
//   请求大小 ≤ 32KB → 固定申请 8 页（32KB）
//     原因：小对象场景下，32KB 足够切分成大量小块，
//     一次申请可以服务很多次 ThreadCache 的 fetchRange，
//     同时 8 页不会给 PageCache 造成太大的单个 Span 占用
//
//   请求大小 > 32KB → 按实际需求向上取整页数
//     例：size=100KB → numPages = ceil(100KB/4KB) = 25 页

void* CentralCache::fetchFromPageCache(size_t size)
{
    // 计算最少需要多少页
    // 例：size=24 字节 → (24+4095)/4096=1 → 1 页理论够，但下面会用 8 页
    size_t numPages = (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

    // 8 页 = 8 × 4096 = 32768 字节 = 32KB
    if (size <= SPAN_PAGES * PageCache::PAGE_SIZE)
    {
        // ≤32KB：统一用 8 页，减少 PageCache 碎片
        return PageCache::getInstance().allocateSpan(SPAN_PAGES);
    }
    else
    {
        // >32KB：按需分配
        return PageCache::getInstance().allocateSpan(numPages);
    }
}

// =========================================================================
// getSpanTracker —— 给定一个内存块地址，找到它所属的 Span 追踪器
// =========================================================================
//
// 查找策略：二分查找（已排序部分）+ 线性扫描（未排序尾部）
//   数组按 spanAddr 升序排列的前 sortedCount_ 个条目用二分（O(log n)），
//   尾部未排序条目（最多 SORT_THRESHOLD=64 个）退化为线性扫描（O(1)）。
//   重排序由 ensureSorted() 触发，使用与扩容相同的原子替换模式。

SpanTracker* CentralCache::getSpanTracker(void* blockAddr)
{
    SpanTracker** array = trackerArray_.load(std::memory_order_acquire);
    size_t sorted = sortedCount_.load(std::memory_order_acquire);
    size_t total = spanCount_.load(std::memory_order_relaxed);

    // ---- 阶段 1：在已排序部分二分查找 ----
    size_t left = 0, right = sorted;
    while (left < right)
    {
        size_t mid = left + (right - left) / 2;
        SpanTracker* t = array[mid];
        void* addr = t->spanAddr.load(std::memory_order_relaxed);

        if (blockAddr < addr)
        {
            right = mid;
        }
        else if (blockAddr >= static_cast<char*>(addr) +
                 t->numPages.load(std::memory_order_relaxed) * PageCache::PAGE_SIZE)
        {
            left = mid + 1;
        }
        else
        {
            return t;  // 命中
        }
    }

    // ---- 阶段 2：在未排序尾部线性扫描 ----
    for (size_t i = sorted; i < total; ++i)
    {
        SpanTracker* t = array[i];
        void* addr = t->spanAddr.load(std::memory_order_relaxed);
        size_t pages = t->numPages.load(std::memory_order_relaxed);

        if (blockAddr >= addr &&
            blockAddr < static_cast<char*>(addr) + pages * PageCache::PAGE_SIZE)
        {
            return t;
        }
    }
    return nullptr;
}

// =========================================================================
// expandTrackerArray —— 动态扩容追踪器数组
// =========================================================================
//
// 扩容流程：
//   1. 获取 expandMutex_（串行化扩容，极低频）
//   2. 分配新的更大的指针数组
//   3. 在 trackerStorage_ 中构造新的 SpanTracker 对象
//   4. 将新旧指针一起填入新数组
//   5. 原子交换 trackerArray_ 和 trackerCount_
//   6. 旧数组泄漏（~8KB，扩容仅发生几次，可接受）
//
// 读路径（getSpanTracker）不受影响：旧数组在交换后仍然有效，
// 已经在读取旧数组的线程可以继续安全遍历。

void CentralCache::expandTrackerArray(size_t requiredIndex)
{
    std::lock_guard<std::mutex> lock(trackerExpandMutex_);

    // 双重检查：可能其他线程已经扩容过了
    if (requiredIndex < trackerCount_.load(std::memory_order_relaxed))
        return;

    size_t oldSize = trackerCount_.load(std::memory_order_relaxed);
    size_t newSize = std::max(oldSize + TRACKER_EXPAND_SIZE, requiredIndex + 1);

    // 分配新的指针数组
    SpanTracker** newArray = new SpanTracker*[newSize];

    // 复制旧指针
    SpanTracker** oldArray = trackerArray_.load(std::memory_order_acquire);
    for (size_t i = 0; i < oldSize; ++i)
    {
        newArray[i] = oldArray[i];
    }

    // 在 trackerStorage_ 中构造新的 SpanTracker，填充新指针
    for (size_t i = oldSize; i < newSize; ++i)
    {
        trackerStorage_.emplace_back();
        newArray[i] = &trackerStorage_.back();
    }

    // 按 spanAddr 排序（Span 地址不重叠，排序后支持二分查找）
    std::sort(newArray, newArray + newSize,
        [](SpanTracker* a, SpanTracker* b) {
            return a->spanAddr.load(std::memory_order_relaxed) <
                   b->spanAddr.load(std::memory_order_relaxed);
        });

    // 原子交换：读路径从这里开始看到新数组
    trackerArray_.store(newArray, std::memory_order_release);
    trackerCount_.store(newSize, std::memory_order_release);
    sortedCount_.store(newSize, std::memory_order_release);  // 新数组全排序

    // 旧数组不释放（可能有读者还在用），由进程退出时 OS 回收
}

// =========================================================================
// ensureSorted —— 未排序条目超阈值时重排序数组
// =========================================================================
//
// 与扩容相同的原子替换模式：分配新数组 → 复制 → 排序 → 原子交换。
// 触发条件：spanCount_ - sortedCount_ > SORT_THRESHOLD (64)

void CentralCache::ensureSorted()
{
    size_t total = spanCount_.load(std::memory_order_relaxed);
    size_t sorted = sortedCount_.load(std::memory_order_relaxed);

    if (total - sorted <= SORT_THRESHOLD)
        return;  // 未超阈值，无需重排

    std::lock_guard<std::mutex> lock(trackerExpandMutex_);

    // 双重检查
    total = spanCount_.load(std::memory_order_relaxed);
    sorted = sortedCount_.load(std::memory_order_relaxed);
    if (total - sorted <= SORT_THRESHOLD)
        return;

    size_t arraySize = trackerCount_.load(std::memory_order_relaxed);

    // 分配新数组
    SpanTracker** newArray = new SpanTracker*[arraySize];

    // 复制所有指针
    SpanTracker** oldArray = trackerArray_.load(std::memory_order_acquire);
    for (size_t i = 0; i < total; ++i)
    {
        newArray[i] = oldArray[i];
    }
    // 剩余未使用的槽位置空
    for (size_t i = total; i < arraySize; ++i)
    {
        newArray[i] = oldArray[i];
    }

    // 对已使用的 [0, total) 部分按 spanAddr 排序
    std::sort(newArray, newArray + total,
        [](SpanTracker* a, SpanTracker* b) {
            return a->spanAddr.load(std::memory_order_relaxed) <
                   b->spanAddr.load(std::memory_order_relaxed);
        });

    // 原子交换
    trackerArray_.store(newArray, std::memory_order_release);
    sortedCount_.store(total, std::memory_order_release);

    // 旧数组泄漏
}

} // namespace Kama_memoryPool
