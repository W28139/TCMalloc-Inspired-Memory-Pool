#pragma once
#include "Common.h"
#include <mutex>
#include <deque>
#include <array>
#include <atomic>
#include <chrono>

// ============================================================================
// CentralCache —— 中心缓存层（三层架构的第二层，全局调度中枢）
// ============================================================================
//
// 承上启下：ThreadCache 来取货（fetchRange），ThreadCache 来退货（returnRange），
// 缺货时从 PageCache 拿新 Span 切分。
//
// ── 三层架构全貌 ──
//
//   [用户代码]
//       ↓ MemoryPool::allocate / deallocate
//   [ThreadCache]   ← thread_local，完全无锁，分配第一站
//       ↓ fetchRange / returnRange
//   [CentralCache]  ← 全局共享，桶级自旋锁（每个大小类别一把锁）
//       ↓ allocateSpan / deallocateSpan
//   [PageCache]     ← 全局共享，互斥锁，mmap/munmap
//       ↓
//   [OS Kernel]
//
// ── 核心数据结构 ──
//
//   centralFreeList_[32768]  — 每个大小类别一个单向链表，存储空闲内存块
//   locks_[32768]            — 每个大小类别一个自旋锁（桶锁）
//
//   SpanTracker — 追踪每个 Span 的空闲情况
//     spanAddr   该 Span 的起始地址
//     numPages   包含的页数
//     blockCount 被切分成的总块数
//     freeCount  当前空闲块数（freeCount == blockCount → 归还 PageCache）
//     scanCount  performDelayedReturn 中的临时计数器（替代 unordered_map）
//
//   trackerStorage_ (deque)            — 持有所有 SpanTracker 对象
//   trackerArray_   (atomic<T**>)      — 扁平指针数组，getSpanTracker 无锁遍历
//   trackerCount_   (atomic<size_t>)   — 数组容量
//   spanCount_      (atomic<size_t>)   — 已使用的 SpanTracker 数量
//   sortedCount_    (atomic<size_t>)   — 已按 spanAddr 排序的条目数（二分查找）
//
// ── 延迟归还机制 ──
//
//   returnRange 不立即检查"Span 是否全空"，而是累加 delayCount_。
//   达到 48 次或距上次检查 1 秒后，触发 performDelayedReturn：
//     1. 遍历 centralFreeList_，在 SpanTracker.scanCount 上累加
//     2. 遍历 trackerArray_，取出 scanCount 并检查 freeCount==blockCount
//     3. 全空闲的 Span 从 centralFreeList_ 摘除，归还 PageCache
//
// ── 关键修复记录 ──
//
//   #01 freeCount 累加→直接设置     (防止重复计数导致 Span 永不归还)
//   #02 freeListSize_ 先减→后减     (防止无符号下溢到 SIZE_MAX)
//   #03 array→deque+无锁指针数组    (支持动态扩容，消除 shared_mutex 开销)
//   #04 单块→批量 BATCH_SIZE=8      (减少 8 倍跨层调用和锁竞争)
//   #06 线性→二分查找               (sortedCount_ + ensureSorted 懒排序)
//   #07 unordered_map→scanCount     (消除自旋锁临界区内的堆分配)
//   #10 blockNum==1 也创建 Tracker   (大块内存正确回收)

namespace Kama_memoryPool
{

// Span 追踪器 —— 每个从 PageCache 分配的 Span 对应一个
// 所有字段为 atomic，支持无锁读取（getSpanTracker）和原子更新
struct SpanTracker {
    std::atomic<void*> spanAddr{nullptr};   // Span 起始地址（页对齐）
    std::atomic<size_t> numPages{0};        // 包含的页数
    std::atomic<size_t> blockCount{0};      // 切分成的总块数
    std::atomic<size_t> freeCount{0};       // 当前在 centralFreeList_ 中的空闲块数
    std::atomic<size_t> scanCount{0};       // performDelayedReturn 临时计数
};

class CentralCache
{
public:
    static CentralCache& getInstance()
    {
        static CentralCache instance;
        return instance;
    }

    // 向 ThreadCache 批量分发内存块（返回 BATCH_SIZE 块的链表头）
    void* fetchRange(size_t index);

    // 接收 ThreadCache 归还的内存块链表
    void returnRange(void* start, size_t size, size_t index);

private:
    CentralCache();

    // 从 PageCache 申请新 Span
    void* fetchFromPageCache(size_t size);

    // 给定块地址，查找所属 Span 的追踪器（已排序部分二分查找 O(log n)）
    SpanTracker* getSpanTracker(void* blockAddr);

    // 更新 Span 空闲计数，全空闲则归还 PageCache
    void updateSpanFreeCount(SpanTracker* tracker, size_t freeBlocksInList, size_t index);

    // 扩容 SpanTracker 数组（每次 +256 槽位）
    void expandTrackerArray(size_t requiredIndex);

    // 重排序数组以支持二分查找（未排序条目超 64 个触发）
    void ensureSorted();

    // 延迟归还判断与执行
    bool shouldPerformDelayedReturn(size_t index, size_t currentCount,
        std::chrono::steady_clock::time_point currentTime);
    void performDelayedReturn(size_t index);

private:
    // ---- 自由链表与锁 ----
    // centralFreeList_[i] → 大小类别 i 的空闲块链表头（单向链表，next 存于块前 8 字节）
    std::array<std::atomic<void*>, FREE_LIST_SIZE> centralFreeList_;
    // locks_[i] 保护 centralFreeList_[i] 的并发访问（桶级自旋锁）
    std::array<std::atomic_flag, FREE_LIST_SIZE> locks_;

    // ---- SpanTracker 无锁存储 ----
    // 两层结构：
    //   1. trackerStorage_ (deque)    — 实际持有 SpanTracker 对象（deque 地址稳定）
    //   2. trackerArray_  (atomic)    — 扁平指针数组，扩容/重排时原子替换
    std::deque<SpanTracker> trackerStorage_;
    std::atomic<SpanTracker**> trackerArray_{nullptr};
    std::atomic<size_t> trackerCount_{0};          // 数组容量（总槽位数）
    std::atomic<size_t> spanCount_{0};             // 已使用的 SpanTracker 数量
    std::atomic<size_t> sortedCount_{0};           // 已按 spanAddr 排序的条目数
    std::mutex trackerExpandMutex_;                // 保护扩容和重排序（极低频）

    // ---- 配置常量 ----
    static const size_t TRACKER_INITIAL_CAPACITY = 1024;  // 初始 1024 个槽位
    static const size_t TRACKER_EXPAND_SIZE = 256;        // 每次扩容 +256
    static const size_t BATCH_SIZE = 8;                   // fetchRange 批量传输 8 块
    static const size_t SORT_THRESHOLD = 64;              // 未排序超 64 触发重排

    // ---- 延迟归还 ----
    static const size_t MAX_DELAY_COUNT = 48;    // 累计归还操作 48 次后触发检查
    std::array<std::atomic<size_t>, FREE_LIST_SIZE> delayCounts_;
    std::array<std::chrono::steady_clock::time_point, FREE_LIST_SIZE> lastReturnTimes_;
    static const std::chrono::milliseconds DELAY_INTERVAL; // 1 秒
};

} // namespace Kama_memoryPool
