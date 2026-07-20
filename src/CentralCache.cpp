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
}

// =========================================================================
// fetchRange —— 向 ThreadCache 分发一批内存块
// =========================================================================
//
// 这是 CentralCache 最核心的"出货"函数。ThreadCache 在本地缓存不足时调用它。
//
// 参数：
//   index - 大小类别索引。例如 index=2 对应 (2+1)*8=24→向上取整到32字节的大小类
//
// 返回值：
//   单个内存块的地址（void*），由 ThreadCache 取走使用
//   同时 centralFreeList_[index] 中还保留剩余块供后续请求使用
//
// 流程概要：
//   ┌─ 检查 index 合法性
//   ├─ 获取自旋锁 locks_[index]
//   ├─ 尝试从 centralFreeList_[index] 取已有的空闲块
//   │   ├─ 有 → 取链表头，更新链表，更新 SpanTracker 计数
//   │   └─ 无 → 从 PageCache 申请新 Span → 切分成小块 → 串成链表 → 取链表头
//   └─ 释放自旋锁，返回取到的块

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

                // --- 从链表中取出第一块返回给 ThreadCache，其余留在 CentralCache ---
                //
                // 此时链表状态：
                //   result(块0) → 块1 → 块2 → ... → nullptr
                //
                // 目标：
                //   result 单独返回给 ThreadCache
                //   块1 → 块2 → ... → nullptr 挂在 centralFreeList_[index] 上

                // 保存块0的 next（即块1的地址）
                void* next = *reinterpret_cast<void**>(result);

                // 将块0的 next 置空（块 0 即将返回给用户，不需要链表连接）
                *reinterpret_cast<void**>(result) = nullptr;

                // 将剩余链表（块1 → 块2 → ... → nullptr）更新到中心缓存
                centralFreeList_[index].store(next, std::memory_order_release);

                // --- 记录 SpanTracker：跟踪这个 Span 的空闲情况 ---
                //
                // 为什么需要 SpanTracker？
                //   1. CentralCache 管理的是切分后的小块，它们物理上可能散布在不同 Span 中
                //   2. PageCache::deallocateSpan 要求归还"连续的整块内存"
                //   3. 只有当一个 Span 的所有小块都归还（freeCount == blockCount），
                //      才能把这个 Span 整体归还给 PageCache
                //
                // spanCount_ 原子递增，既拿到当前槽位号，又为下一次分配预留
                size_t trackerIndex = spanCount_++;

                if (trackerIndex < spanTrackers_.size())
                {
                    // spanAddr：这个 Span 在内存中的起始地址
                    spanTrackers_[trackerIndex].spanAddr.store(start, std::memory_order_release);
                    // numPages：这个 Span 包含多少页
                    spanTrackers_[trackerIndex].numPages.store(numPages, std::memory_order_release);
                    // blockCount：这个 Span 被切分成的总块数
                    spanTrackers_[trackerIndex].blockCount.store(blockNum, std::memory_order_release);
                    // freeCount：初始空闲块数 = blockNum - 1
                    // 因为第一块（result）已被取走，不计算在"空闲"中
                    spanTrackers_[trackerIndex].freeCount.store(blockNum - 1, std::memory_order_release);
                }
            }
            // 注：如果 blockNum <= 1（理论上不太会发生），result 就是唯一一块，
            // 不需要构建链表，也不需要 SpanTracker。直接返回就行。
        }
        else
        {
            // ================================================
            // 分支 B：中心缓存有现成的空闲块 → 直接取链表头
            // ================================================

            // 此时链表状态：
            //   result(头节点) → 块X → 块Y → ... → nullptr
            //
            // 目标：
            //   result 返回给 ThreadCache
            //   块X → 块Y → ... → nullptr 更新为新的链表头

            // 保存链表第二个节点的地址（块X）
            void* next = *reinterpret_cast<void**>(result);

            // 断开 result 与链表的连接
            *reinterpret_cast<void**>(result) = nullptr;

            // 将剩余链表更新到中心缓存
            centralFreeList_[index].store(next, std::memory_order_release);

            // --- 更新 SpanTracker：这个块被取走了，空闲计数 -1 ---
            //
            // getSpanTracker 做的事：
            //   遍历 spanTrackers_，找到 result 这个地址属于哪个 Span
            //   判断方式：spanAddr ≤ result < spanAddr + numPages * PAGE_SIZE
            SpanTracker* tracker = getSpanTracker(result);
            if (tracker)
            {
                // fetch_sub：原子地将 freeCount 减 1
                // 例：之前 5 块空闲，取走 1 块 → freeCount 变为 4
                tracker->freeCount.fetch_sub(1, std::memory_order_release);
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

    // --- 遍历空闲链表，按 Span 分组统计 ---
    //
    // 为什么需要 std::unordered_map？
    //   中心缓存的空闲链表可能混合了来自不同 Span 的内存块：
    //     centralFreeList_[2] → [SpanA的块1] → [SpanB的块5] → [SpanA的块3] → ...
    //   必须按 Span 分组后，才能知道每个 Span 分别有多少块空闲
    //
    // spanFreeCounts 的结构：
    //   { SpanTrackerA* → 该 Span 在空闲链表中出现的次数, ... }
    std::unordered_map<SpanTracker*, size_t> spanFreeCounts;

    // 从头遍历空闲链表
    void* currentBlock = centralFreeList_[index].load(std::memory_order_relaxed);

    while (currentBlock)
    {
        // 查找当前块属于哪个 Span
        SpanTracker* tracker = getSpanTracker(currentBlock);
        if (tracker)
        {
            // 该 Span 的空闲计数 +1
            spanFreeCounts[tracker]++;
        }
        // 移动到链表中的下一块
        currentBlock = *reinterpret_cast<void**>(currentBlock);
    }

    // --- 逐个 Span 更新空闲计数并判断是否可归还 ---
    // C++17 结构化绑定：直接解包 unordered_map 的 key 和 value
    for (const auto& [tracker, newFreeBlocks] : spanFreeCounts)
    {
        updateSpanFreeCount(tracker, newFreeBlocks, index);
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

void CentralCache::updateSpanFreeCount(SpanTracker* tracker, size_t newFreeBlocks, size_t index)
{
    // --- 第 1 步：更新空闲计数 ---
    // 注意：freeCount 跟踪的是 Span 创建以来的累计空闲块数
    // newFreeBlocks 是本次扫描在链表中找到的块数
    size_t oldFreeCount = tracker->freeCount.load(std::memory_order_relaxed);
    size_t newFreeCount = oldFreeCount + newFreeBlocks;
    tracker->freeCount.store(newFreeCount, std::memory_order_release);

    // --- 第 2 步：判断是否全部空闲 ---
    // 全部空闲的判定：freeCount == blockCount
    //   blockCount = 该 Span 总共被切分成的块数
    //   freeCount  = 累计回到中心缓存的块数
    if (newFreeCount == tracker->blockCount.load(std::memory_order_relaxed))
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
// 这是 CentralCache 的"反向查找"函数：
//   已知：一个切分后的小块地址（如 0x4280）
//   求解：这个块最初是从哪个 Span 切出来的
//
// 查找方式：遍历 spanTrackers_ 数组（最多 1024 项），
// 检查 blockAddr 是否在某个 Span 的地址范围内
//
// 复杂度：O(n)，n = spanCount_（当前活跃的 Span 数量，最多 1024）
// 为什么不用 map？避免 map 本身依赖 malloc，造成循环依赖

SpanTracker* CentralCache::getSpanTracker(void* blockAddr)
{
    // 遍历所有已注册的 SpanTracker
    for (size_t i = 0; i < spanCount_.load(std::memory_order_relaxed); ++i)
    {
        // 读取该 Span 的起始地址和页数
        void* spanAddr = spanTrackers_[i].spanAddr.load(std::memory_order_relaxed);
        size_t numPages = spanTrackers_[i].numPages.load(std::memory_order_relaxed);

        // 判断 blockAddr 是否在这个 Span 的地址范围内
        // 范围：[spanAddr, spanAddr + numPages * 4096)
        if (blockAddr >= spanAddr &&
            blockAddr < static_cast<char*>(spanAddr) + numPages * PageCache::PAGE_SIZE)
        {
            // 找到了！返回该 SpanTracker 的地址
            return &spanTrackers_[i];
        }
    }
    // 未找到：blockAddr 不属于任何已记录的 Span
    // 这可能发生在：1) 大对象直接走 malloc 的路径  2) 编程错误
    return nullptr;
}

} // namespace Kama_memoryPool
