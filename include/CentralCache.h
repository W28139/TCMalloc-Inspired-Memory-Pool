#pragma once
#include "Common.h"
#include <mutex>
#include <deque>
#include <array>
#include <atomic>
#include <chrono>

namespace Kama_memoryPool
{

// 使用无锁的span信息存储
struct SpanTracker {
    std::atomic<void*> spanAddr{nullptr};
    std::atomic<size_t> numPages{0};
    std::atomic<size_t> blockCount{0};
    std::atomic<size_t> freeCount{0};   // 空闲块数
    std::atomic<size_t> scanCount{0};   // performDelayedReturn 临时计数，替代 unordered_map
};

class CentralCache
{
public:
    static CentralCache& getInstance()
    {
        static CentralCache instance;
        return instance;
    }

    void* fetchRange(size_t index);
    void returnRange(void* start, size_t size, size_t index);

private:
    // 相互是还所有原子指针为nullptr
    CentralCache();
    // 从页缓存获取内存
    void* fetchFromPageCache(size_t size);

    // 获取span信息
    SpanTracker* getSpanTracker(void* blockAddr);

    // 更新span的空闲计数并检查是否可以归还
    void updateSpanFreeCount(SpanTracker* tracker, size_t newFreeBlocks, size_t index);

    // 扩容 span 追踪器数组
    void expandTrackerArray(size_t requiredIndex);
    // 重排序数组以支持二分查找
    void ensureSorted();

private:
    // 中心缓存的自由链表
    std::array<std::atomic<void*>, FREE_LIST_SIZE> centralFreeList_;

    // 用于同步的自旋锁
    std::array<std::atomic_flag, FREE_LIST_SIZE> locks_;

    // SpanTracker 存储与无锁访问
    //   两层结构：
    //     1. trackerStorage_ (deque) — 持有 SpanTracker 对象，deque 保证地址稳定
    //     2. trackerArray_  (atomic<SpanTracker**>) — 扁平指针数组，getSpanTracker 无锁遍历
    //   扩容时分配新数组、复制旧指针、原子交换；读路径仅需两次 atomic load
    std::deque<SpanTracker> trackerStorage_;
    std::atomic<SpanTracker**> trackerArray_{nullptr};
    std::atomic<size_t> trackerCount_{0};
    std::atomic<size_t> spanCount_{0};
    std::mutex trackerExpandMutex_;                       // 仅保护扩容（极低频）

    static const size_t TRACKER_INITIAL_CAPACITY = 1024;  // 初始容量
    static const size_t TRACKER_EXPAND_SIZE = 256;        // 每次扩容槽位数
    static const size_t BATCH_SIZE = 8;                   // fetchRange 每次批量传输块数
    static const size_t SORT_THRESHOLD = 64;              // 未排序条目超此阈值触发重排
    std::atomic<size_t> sortedCount_{0};                  // 已按 spanAddr 排序的条目数

    // 延迟归还相关的成员变量
    static const size_t MAX_DELAY_COUNT = 48;  // 最大延迟计数
    std::array<std::atomic<size_t>, FREE_LIST_SIZE> delayCounts_;  // 每个大小类的延迟计数
    std::array<std::chrono::steady_clock::time_point, FREE_LIST_SIZE> lastReturnTimes_;  // 上次归还时间
    static const std::chrono::milliseconds DELAY_INTERVAL;  // 延迟间隔

    bool shouldPerformDelayedReturn(size_t index, size_t currentCount, std::chrono::steady_clock::time_point currentTime);
    void performDelayedReturn(size_t index);
};

} // namespace memoryPool