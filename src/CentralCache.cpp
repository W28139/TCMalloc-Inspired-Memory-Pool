/**
 * @file    CentralCache.cpp
 * @brief   中心缓存层 —— 全局共享的内存调度中枢
 *
 * ── CentralCache 在三层架构中的位置 ──
 *
 *   [ThreadCache]   ← 线程私有，无锁（用户每线程一个 ThreadCache）
 *        ↕  fetchRange / returnRange
 *   [CentralCache]  ← 全局共享，桶级自旋锁 ← 本文件
 *        ↕  allocateSpan / deallocateSpan
 *   [PageCache]     ← 全局共享，互斥锁，mmap/munmap
 *
 * ── 核心职责 ──
 *
 *   1. 向 ThreadCache 批量分发内存块（fetchRange）
 *   2. 接收 ThreadCache 归还的多余内存块（returnRange）
 *   3. 内存不足时从 PageCache 申请新 Span，切分成小块
 *   4. 跟踪每个 Span 的空闲块数量，整 Span 空闲时归还 PageCache
 *   5. 延迟归还：批量检查而非每次归还都扫描
 *
 * ── 关键修复记录 ──
 *
 *   #01 (freeCount):   累加→直接设置      防止重复计数永不归还
 *   #03 (spanTrackers): array→动态扩容      支持超过1024个Span
 *   #04 (批量传输):     单块→BATCH_SIZE=8   减少8倍跨层调用
 *   #06 (二分查找):     线性→懒排序+二分     O(n)→O(log n)
 *   #07 (unordered_map): map→scanCount字段  消除自旋锁内堆分配
 *   #10 (blockNum==1):  补SpanTracker        大块内存正确回收
 */

#include "../include/CentralCache.h"
#include "../include/PageCache.h"
#include <cassert>
#include <thread>
#include <chrono>
#include <algorithm>  // std::sort

namespace wevix_memoryPool
{

// =========================================================================
// 静态常量定义
// =========================================================================

// 延迟归还时间间隔：1 秒
// 一个大小类别至少隔 1 秒才触发一次 Span 归还检查，防止频繁归还/申请抖动
const std::chrono::milliseconds CentralCache::DELAY_INTERVAL{1000};

// 每次向 PageCache 申请 Span 时默认申请 8 页（32KB）
// ≤32KB 的大小类别统一用 8 页，减少 PageCache 碎片
static const size_t SPAN_PAGES = 8;

// =========================================================================
// 构造函数
// =========================================================================
//
// 初始化所有数据结构：
//   1. centralFreeList_[32768] → 全 nullptr（无空闲块）
//   2. [locks_32768]           → 全 clear（未锁定）
//   3. delayCounts_[32768]     → 全 0
//   4. lastReturnTimes_[32768] → 全设为"现在"（防止刚启动就触发归还）
//   5. spanCount_              → 0
//   6. trackerStorage_ + trackerArray_ → 预分配 1024 个 SpanTracker

CentralCache::CentralCache()
{
    // 初始化所有自由链表为空
    for (auto& ptr : centralFreeList_)
    {
        ptr.store(nullptr, std::memory_order_relaxed);
    }

    // 初始化所有自旋锁为未锁定
    for (auto& lock : locks_)
    {
        lock.clear();
    }

    // 初始化延迟归还：计数归零，时间戳置为现在
    for (auto& count : delayCounts_)
    {
        count.store(0, std::memory_order_relaxed);
    }
    for (auto& time : lastReturnTimes_)
    {
        time = std::chrono::steady_clock::now();
    }

    // 预分配 1024 个 SpanTracker，构建无锁指针数组
    // 这里分配了一个堆上的指针数组充当“目录”，它本身不存储数据，而是存储指向实际 SpanTracker 对象的指针。
    spanCount_.store(0, std::memory_order_relaxed);
    SpanTracker** initialArray = new SpanTracker*[TRACKER_INITIAL_CAPACITY];
    for (size_t i = 0; i < TRACKER_INITIAL_CAPACITY; ++i)
    {
        trackerStorage_.emplace_back();     // 在真正的存储空间里创建一个 SpanTracker 对象。
        initialArray[i] = &trackerStorage_.back();  // 把刚创建的那个对象的地址，填进索引表里
    }
    trackerArray_.store(initialArray, std::memory_order_release);
    trackerCount_.store(TRACKER_INITIAL_CAPACITY, std::memory_order_release);
}

// =========================================================================
// fetchRange —— 向 ThreadCache 批量分发内存块（最核心的热路径）
// =========================================================================
//
// 参数：index — 大小类别索引（如 index=2 对应实际块大小 24B）
// 返回：最多 BATCH_SIZE(8) 块内存组成的链表头，ThreadCache 取第 1 块，
//       剩余 7 块进入本地 freeList_
//
// 流程：
//   ┌─ 检查 index 合法性
//   ├─ 获取自旋锁 locks_[index]（桶锁）
//   ├─ 检查 centralFreeList_[index]
//   │   ├─ 为空（分支 A）→ PageCache 申请新 Span → 切分 → 批量取 BATCH_SIZE 块
//   │   └─ 有货（分支 B）→ 批量取 BATCH_SIZE 块 → 逐块更新 SpanTracker.freeCount
//   └─ 释放自旋锁

void* CentralCache::fetchRange(size_t index)
{
    if (index >= FREE_LIST_SIZE)
        return nullptr;

    // 获取桶级自旋锁
    while (locks_[index].test_and_set(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }

    void* result = nullptr;

    try
    {
        result = centralFreeList_[index].load(std::memory_order_relaxed);

        if (!result)
        {
            // ============================================================
            // 分支 A：CentralCache 为空 → 从 PageCache 申请新 Span
            // ============================================================

            size_t size = (index + 1) * ALIGNMENT;

            // 向 PageCache 申请 Span（返回页对齐的连续内存起始地址）
            result = fetchFromPageCache(size);
            if (!result)
            {
                locks_[index].clear(std::memory_order_release);
                return nullptr;
            }

            char* start = static_cast<char*>(result);

            // 计算页数和可切分块数
            size_t numPages = (size <= SPAN_PAGES * PageCache::PAGE_SIZE) ?
                                     SPAN_PAGES :
                                     (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

            size_t blockNum = (numPages * PageCache::PAGE_SIZE) / size;

            if (blockNum > 1)
            {
                // ---- 切分 Span 为 blockNum 个 size 大小的小块，串成单向链表 ----
                //
                // 内存布局（size=32B）：
                //   [块0: 0x1000] [块1: 0x1020] [块2: 0x1040] ...
                // 链表：块0.next=块1, 块1.next=块2, ..., 块N.next=nullptr
                for (size_t i = 1; i < blockNum; ++i)
                {
                    void* current = start + (i - 1) * size;
                    void* next    = start + i * size;
                    *reinterpret_cast<void**>(current) = next;
                }
                *reinterpret_cast<void**>(start + (blockNum - 1) * size) = nullptr;

                // ---- 批量传输：取前 BATCH_SIZE 块返回，剩余留在 CentralCache ----
                // 传输的块数
                size_t batchCount = 1;
                // 传输的尾指针
                void* batchTail = result;
                while (batchCount < BATCH_SIZE && batchCount < blockNum &&
                       *reinterpret_cast<void**>(batchTail) != nullptr)
                {
                    batchTail = *reinterpret_cast<void**>(batchTail);
                    batchCount++;
                }
                // rest为断开后的第一个块地址，用于接入centralFreeList_
                void* rest = *reinterpret_cast<void**>(batchTail);
                *reinterpret_cast<void**>(batchTail) = nullptr; // 在批次尾断开

                // 把申请来的块 - 传入上层的8块 的第一个块rest接入空的centralFreeList_
                centralFreeList_[index].store(rest, std::memory_order_release);

                // 下面就是维护内容了
                // 1. 创建 SpanTracker（登记新领取的内存块）
                // 拿到型假span的编号
                size_t trackerIndex = spanCount_++;
                
                // 2. 检查索引表（目录）够不够大。
                // trackerCount_ 是当前指针数组（目录表）的最大容量（初始1024）。
                // 如果领到的号超过了目录大小，就得给目录扩容。
                if (trackerIndex >= trackerCount_.load(std::memory_order_relaxed))
                    expandTrackerArray(trackerIndex);

                // 3. 获取当前的目录表指针,array
                // 使用 acquire 语义确保能看到 expandTrackerArray 中最新分配的内存。    
                SpanTracker** array = trackerArray_.load(std::memory_order_acquire);

                // 4. 从目录中通过索引找到对应的“空档案袋”对象（SpanTracker）。
                // 注意：这些对象是在初始化或扩容时就 new 好的，现在直接拿地址用。
                SpanTracker* tracker = array[trackerIndex];

                // 5. 将这块内存的具体信息（Span 的属性）填入档案袋中。
                // 使用 store(..., release) 保证其他线程在 getSpanTracker 查到这些值时，数据已完整写入。
                tracker->spanAddr.store(start, std::memory_order_release);
                tracker->numPages.store(numPages, std::memory_order_release);
                tracker->blockCount.store(blockNum, std::memory_order_release);

                // 6. 初始化剩余块数（freeCount）。
                // blockNum 是总数，batchCount 是刚才已经分给 ThreadCache 的块数。
                // 修复 #01：这里直接计算剩余值，不再进行累加，避免脏数据。
                tracker->freeCount.store(blockNum - batchCount, std::memory_order_release);

                // ---- 性能优化：维护索引的有序性（修复 #06） ----
                // 7. 获取当前页总数和已经排好序的数量。
                size_t total = spanCount_.load(std::memory_order_relaxed);
                size_t sorted = sortedCount_.load(std::memory_order_relaxed);
                // 8. 核心策略：懒排序触发器
                // 如果（总数 - 已排序数） > 64，说明末尾乱序的部分太长了，会拖慢 getSpanTracker 的线性扫描速度。
                // 这时调用 ensureSorted() 将所有 Span 按地址排序，以便后续能用二分查找（O(log n)）。
                if (total > sorted && (total - sorted) > SORT_THRESHOLD)
                    ensureSorted();
            }
            // 只有一个块（size>=16kb的操作）
            else
            {
                // blockNum == 1：大对象场景（修复 #10）
                // 虽然只有一个块无需链表，但必须创建 SpanTracker
                // 否则该 Span 永远无法归还 PageCache
                // 直接维护trackerArray_即可，逻辑与上一致
                size_t trackerIndex = spanCount_++;

                if (trackerIndex >= trackerCount_.load(std::memory_order_relaxed))
                    expandTrackerArray(trackerIndex);

                SpanTracker** array = trackerArray_.load(std::memory_order_acquire);
                SpanTracker* tracker = array[trackerIndex];

                tracker->spanAddr.store(start, std::memory_order_release);
                tracker->numPages.store(numPages, std::memory_order_release);
                tracker->blockCount.store(1, std::memory_order_release);
                // freeCount=0：唯一一块已返回给 ThreadCache
                tracker->freeCount.store(0, std::memory_order_release);

                size_t total = spanCount_.load(std::memory_order_relaxed);
                size_t sorted = sortedCount_.load(std::memory_order_relaxed);
                if (total > sorted && (total - sorted) > SORT_THRESHOLD)
                    ensureSorted();
            }
        }

        else
        {
            // ============================================================
            // 分支 B：CentralCache 有现成空闲块 → 批量取 BATCH_SIZE 块
            // ============================================================

            // 找出批次尾节点（第 BATCH_SIZE 块或链表末尾）
            size_t batchCount = 1;
            void* batchTail = result;
            while (batchCount < BATCH_SIZE &&
                   *reinterpret_cast<void**>(batchTail) != nullptr)
            {
                batchTail = *reinterpret_cast<void**>(batchTail);
                batchCount++;
            }

            // 保存批次后的剩余链表，在批次尾断开
            void* rest = *reinterpret_cast<void**>(batchTail);
            *reinterpret_cast<void**>(batchTail) = nullptr;
            centralFreeList_[index].store(rest, std::memory_order_release);

            // 逐块更新 freeCount（批次内块可能来自不同 Span）
            void* current = result;
            for (size_t i = 0; i < batchCount; ++i)
            {
                SpanTracker* tracker = getSpanTracker(current);
                if (tracker)
                    tracker->freeCount.fetch_sub(1, std::memory_order_release);
                current = *reinterpret_cast<void**>(current);
            }
        }
    }
    catch (...)
    {
        locks_[index].clear(std::memory_order_release);
        throw;
    }

    locks_[index].clear(std::memory_order_release);
    return result;
}

// =========================================================================
// returnRange —— 接收 ThreadCache 归还的多余内存块
// =========================================================================
//
// ThreadCache 超 256 阈值时，把超出的 3/4 串成链表归还。
// 使用"头插法"将归还链表整体插入 centralFreeList_ 前面。
// 每次归还累加 delayCount_，达到阈值后触发延迟检查。

void CentralCache::returnRange(void* start, size_t size, size_t index)
{
    if (!start || index >= FREE_LIST_SIZE)
        return;

    size_t blockSize = (index + 1) * ALIGNMENT;
    size_t blockCount = size / blockSize;

    // 获取桶级自旋锁
    while (locks_[index].test_and_set(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }

    try
    {
        // ---- 找到归还链表的尾节点 ----
        void* end = start;
        size_t count = 1;
        while (*reinterpret_cast<void**>(end) != nullptr && count < blockCount)
        {
            end = *reinterpret_cast<void**>(end);
            count++;
        }

        // ---- 头插法：CentralCache 现有链表接在归还链表后面 ----
        //
        // 合并前：归还链表 start→...→end→nullptr  |  CentralCache oldHead→...
        // 合并后：start→...→end→oldHead→...
        void* current = centralFreeList_[index].load(std::memory_order_relaxed);
        *reinterpret_cast<void**>(end) = current;
        centralFreeList_[index].store(start, std::memory_order_release);

        // ---- 延迟归还计数 ----
        size_t currentCount = delayCounts_[index].fetch_add(1, std::memory_order_relaxed) + 1;
        auto currentTime = std::chrono::steady_clock::now();

        // 两个条件满足其一就触发归还检查：
        //   A. 累计归还 ≥ 48 次（高频归还）
        //   B. 距上次检查 ≥ 1 秒（定时检查，防止 Span 长期空闲）
        if (shouldPerformDelayedReturn(index, currentCount, currentTime))
        {
            performDelayedReturn(index);
        }
    }
    catch (...)
    {
        locks_[index].clear(std::memory_order_release);
        throw;
    }

    locks_[index].clear(std::memory_order_release);
}

// =========================================================================
// shouldPerformDelayedReturn —— 判断是否触发延迟归还检查
// =========================================================================
//
// 为什么延迟？归还操作很频繁，如果每次归还都遍历链表统计 Span 空闲情况，
// 代价太高。设置宽松的触发条件，批量处理。

bool CentralCache::shouldPerformDelayedReturn(size_t index, size_t currentCount,
    std::chrono::steady_clock::time_point currentTime)
{
    if (currentCount >= MAX_DELAY_COUNT)   // 累计 ≥ 48 次
        return true;

    auto lastTime = lastReturnTimes_[index];
    return (currentTime - lastTime) >= DELAY_INTERVAL; // 距上次 ≥ 1 秒
}

// =========================================================================
// performDelayedReturn —— 找出完全空闲的 Span 归还 PageCache
// =========================================================================
//
// 修复 #07：两遍扫描替代 unordered_map，消除自旋锁临界区内的堆分配。
//
// 第一遍：遍历 centralFreeList_，在 SpanTracker.scanCount 上原子累加
// 第二遍：遍历 trackerArray_，取出 scanCount → updateSpanFreeCount
//
// 如果 freeCount == blockCount（所有块都空闲），则：
//   1. 从 centralFreeList_ 摘除属于该 Span 的所有块（防止悬垂指针）
//   2. PageCache::deallocateSpan 归还整块连续内存

void CentralCache::performDelayedReturn(size_t index)
{
    // 1. 【重置计数器与时间戳】
    delayCounts_[index].store(0, std::memory_order_relaxed);
    lastReturnTimes_[index] = std::chrono::steady_clock::now();

    // ---- 第一遍：遍历空闲链表，在 SpanTracker 上累加计数 ----
    // 遍历链表centralFreeList_，弄清楚这些乱序的内存块分别属于哪几个 Span
    // 获取当前大小类对应的空闲块链表头结点
    void* currentBlock = centralFreeList_[index].load(std::memory_order_relaxed);
    while (currentBlock)
    {
        // 根据内存块的地址，找到它所属的“管理档案”（SpanTracker）
        // 这里会用到之前优化过的 O(log n) 二分查找
        SpanTracker* tracker = getSpanTracker(currentBlock);
        if (tracker)
        {
            // 找到所属 Span 后，在它内部的临时计数器 scanCount 上加 1
            // scanCount 是一个 atomic 变量，保证了即使多个线程都在执行归还，计数也是准确的
            // 使用 memory_order_relaxed 因为这里只保证原子增减，暂不涉及复杂的内存屏障
            tracker->scanCount.fetch_add(1, std::memory_order_relaxed);
        }
        currentBlock = *reinterpret_cast<void**>(currentBlock);
    }

    // ---- 第二遍：遍历所有 SpanTracker（span)，处理有空闲块的 ----
    // 获取当前的索引表（目录）地址
    SpanTracker** array = trackerArray_.load(std::memory_order_acquire);
    // 获取当前总共有多少个 Span
    size_t total = spanCount_.load(std::memory_order_relaxed);

    for (size_t i = 0; i < total; ++i)
    {
        SpanTracker* tracker = array[i];
        // 【原子取出并清零】
        // 使用 exchange(0) 一次性完成两个动作：
        // a. 取出刚才在 while 循环里累加的总数,让count = scanCount(也就是该span的空闲块数)
        // b. 将 scanCount 重置为 0，以便迎接下一次 performDelayedReturn
        size_t count = tracker->scanCount.exchange(0, std::memory_order_relaxed);
        // 如果这个 Span 在刚才的扫描中有贡献（即 count > 0）
        if (count > 0)
        {
            // 调用核心更新逻辑：将这 count 个块真正还给这个 Span
            // 这个函数内部会处理：增加 Span 的 freeCount、判断是否要把全空的 Span 还给 PageCache 等
            updateSpanFreeCount(tracker, count, index);
        }
    }
}

// =========================================================================
// updateSpanFreeCount —— 更新 Span 空闲计数，全空闲时归还 PageCache
// =========================================================================
//
// 修复 #01：从累加（oldFreeCount + newFreeBlocks）改为直接设置（freeBlocksInList）。
// 因为 performDelayedReturn 每次扫描 centralFreeList_ 当前所有块，
// 累加会导致同一批块被重复计数，freeCount 超过 blockCount，永不归还。
//
// 归还时：
//   1. 遍历 centralFreeList_，摘除属于该 Span 的所有块
//   2. PageCache::deallocateSpan 归还连续内存

void CentralCache::updateSpanFreeCount(SpanTracker* tracker, size_t freeBlocksInList, size_t index)
{
    // 直接设置为本次扫描统计到的空闲块数（修复 #01：从累加改为直接设置）
    tracker->freeCount.store(freeBlocksInList, std::memory_order_release);

    // 全部空闲（freeCount == blockCount）→ 归还 Span
    if (freeBlocksInList == tracker->blockCount.load(std::memory_order_relaxed))
    {
        void* spanAddr = tracker->spanAddr.load(std::memory_order_relaxed);
        size_t numPages = tracker->numPages.load(std::memory_order_relaxed);

        // ---- 从 centralFreeList_ 摘除属于该 Span 的所有块 ----
        // 必须摘除：否则后续 fetchRange 可能访问已释放的内存（悬垂指针）
        void* head = centralFreeList_[index].load(std::memory_order_relaxed);
        void* newHead = nullptr;
        void* prev = nullptr;
        void* current = head;

        while (current)
        {
            void* next = *reinterpret_cast<void**>(current);

            // 判断 current 是否属于要归还的 Span
            if (current >= spanAddr &&
                current < static_cast<char*>(spanAddr) + numPages * PageCache::PAGE_SIZE)
            {
                // 属于 → 跳过（从链表中移除）
                if (prev)
                    *reinterpret_cast<void**>(prev) = next;
                else
                    newHead = next;
                // prev 不变（当前节点被跳过）
            }
            else
            {
                // 不属于 → 保留在新链表中
                prev = current;
            }
            current = next;
        }

        centralFreeList_[index].store(newHead, std::memory_order_release);

        // 归还 Span 给 PageCache（可能被合并、缓存或 munmap）
        PageCache::getInstance().deallocateSpan(spanAddr, numPages);
    }
}

// =========================================================================
// fetchFromPageCache —— 向 PageCache 申请新 Span
// =========================================================================
//
// 策略：
//   请求 ≤ 32KB → 固定申请 8 页（减少 PageCache 碎片）
//   请求 > 32KB → 按实际需要向上取整

void* CentralCache::fetchFromPageCache(size_t size)
{
    size_t numPages = (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

    if (size <= SPAN_PAGES * PageCache::PAGE_SIZE)
    {
        return PageCache::getInstance().allocateSpan(SPAN_PAGES); // 8 页
    }
    else
    {
        return PageCache::getInstance().allocateSpan(numPages);
    }
}

// =========================================================================
// getSpanTracker —— 给定块地址，查找所属 Span 的追踪器
// =========================================================================
//
// 修复 #06：已排序部分二分查找 O(log n)，未排序尾部线性扫描 O(SORT_THRESHOLD)。
// 排序由 ensureSorted() 维护，使用与扩容相同的原子替换模式。
//
// 无锁安全：扩容/重排时分配新数组+原子交换，旧数组不变，读者安全。

SpanTracker* CentralCache::getSpanTracker(void* blockAddr)
{
    SpanTracker** array = trackerArray_.load(std::memory_order_acquire);
    size_t sorted = sortedCount_.load(std::memory_order_acquire);
    size_t total = spanCount_.load(std::memory_order_relaxed);

    // ---- 阶段 1：在已排序部分二分查找 O(log n) ----
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
            return t; // 命中
        }
    }

    // ---- 阶段 2：未排序尾部线性扫描 O(SORT_THRESHOLD) ----
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
// expandTrackerArray —— 动态扩容 SpanTracker 数组（修复 #03）
// =========================================================================
//
// 初始容量 1024，每次扩容 +256。扩容、排序后原子替换指针数组。
// 无锁安全：旧数组不释放（可能有读者在用），进程退出时 OS 回收。

void CentralCache::expandTrackerArray(size_t requiredIndex)
{
    std::lock_guard<std::mutex> lock(trackerExpandMutex_);

    // 双重检查：可能其他线程已经扩容过了
    if (requiredIndex < trackerCount_.load(std::memory_order_relaxed))
        return;

    size_t oldSize = trackerCount_.load(std::memory_order_relaxed);
    size_t newSize = std::max(oldSize + TRACKER_EXPAND_SIZE, requiredIndex + 1);

    // 分配新指针数组
    SpanTracker** newArray = new SpanTracker*[newSize];

    // 复制旧指针
    SpanTracker** oldArray = trackerArray_.load(std::memory_order_acquire);
    for (size_t i = 0; i < oldSize; ++i)
        newArray[i] = oldArray[i];

    // 构造新的 SpanTracker
    for (size_t i = oldSize; i < newSize; ++i)
    {
        trackerStorage_.emplace_back();
        newArray[i] = &trackerStorage_.back();
    }

    // 按 spanAddr 排序，支持二分查找（修复 #06）
    std::sort(newArray, newArray + newSize,
        [](SpanTracker* a, SpanTracker* b) {
            return a->spanAddr.load(std::memory_order_relaxed) <
                   b->spanAddr.load(std::memory_order_relaxed);
        });

    // 原子交换
    trackerArray_.store(newArray, std::memory_order_release);
    trackerCount_.store(newSize, std::memory_order_release);
    sortedCount_.store(newSize, std::memory_order_release);
}

// =========================================================================
// ensureSorted —— 未排序条目超阈值时重排序数组（修复 #06）
// =========================================================================
//
// 触发条件：spanCount_ - sortedCount_ > SORT_THRESHOLD(64)
// 与扩容相同的原子替换模式：分配新数组 → 复制 → 排序 → 原子交换。

void CentralCache::ensureSorted()
{
    size_t total = spanCount_.load(std::memory_order_relaxed);
    size_t sorted = sortedCount_.load(std::memory_order_relaxed);

    if (total - sorted <= SORT_THRESHOLD)
        return;

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
        newArray[i] = oldArray[i];
    for (size_t i = total; i < arraySize; ++i)
        newArray[i] = oldArray[i];

    // 对已使用部分按 spanAddr 排序
    std::sort(newArray, newArray + total,
        [](SpanTracker* a, SpanTracker* b) {
            return a->spanAddr.load(std::memory_order_relaxed) <
                   b->spanAddr.load(std::memory_order_relaxed);
        });

    // 原子交换
    trackerArray_.store(newArray, std::memory_order_release);
    sortedCount_.store(total, std::memory_order_release);
}

} // namespace wevix_memoryPool
