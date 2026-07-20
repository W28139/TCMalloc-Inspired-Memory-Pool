# freeCount 重复计数导致 Span 永不归还 PageCache

## 严重程度

**Bug —— 导致内存泄漏**

## 位置

`src/CentralCache.cpp` `performDelayedReturn` + `updateSpanFreeCount`

## 问题描述

`updateSpanFreeCount` 使用**累加**方式更新 `freeCount`：

```cpp
size_t oldFreeCount = tracker->freeCount.load(...);
size_t newFreeCount = oldFreeCount + newFreeBlocks;  // ← 累加！
tracker->freeCount.store(newFreeCount, ...);
```

但 `performDelayedReturn` 每次扫描时，计数的块**可能包含上一次扫描已经在链表中的块**（如果它们没被 ThreadCache 取走的话）。这导致同一批空闲块被反复计数，`freeCount` 虚高。

## 复现路径

```
初始: Span A, blockCount=100, freeCount=99

Step 1: 20 块归还到 centralFreeList_
         centralFreeList_[2] 中 Span A 的块: 20 个

Step 2: performDelayedReturn 扫描
         找到 20 块 → freeCount = 99 + 20 = 119
         119 > 100 ！已经超过 blockCount

         但由于 119 != 100（等于判断），归还逻辑不触发
         这 20 块仍在 centralFreeList_ 中

Step 3: 没有 ThreadCache 来取这个大小的块
         10 块归还到 centralFreeList_
         centralFreeList_[2] 中 Span A 的块: 30 个（含上轮的 20 个）

Step 4: performDelayedReturn 再次扫描
         找到 30 块 → freeCount = 119 + 30 = 149
         仍然 ≠ 100，永远不会触发归还

结论: freeCount 永远无法等于 blockCount → Span 永不归还 → 内存泄漏
```

## 修复方案

将累加改为**直接设置**——`performDelayedReturn` 扫描到的块数就是当前在 `centralFreeList_` 中的块数，直接覆盖：

```cpp
void CentralCache::updateSpanFreeCount(SpanTracker* tracker, size_t freeBlocksInList, size_t index)
{
    // 直接设置为本次扫描统计到的空闲块数（而非累加）
    tracker->freeCount.store(freeBlocksInList, std::memory_order_release);

    if (freeBlocksInList == tracker->blockCount.load(std::memory_order_relaxed))
    {
        // 归还整 Span ...
    }
}
```

**但是**，这样修改后有一个新问题：`freeCount` 只统计 `centralFreeList_` 中的块，不包含仍在 ThreadCache 本地 `freeList_` 中缓存着的块。如果 ThreadCache 还持有该 Span 的块，`freeCount` 永远达不到 `blockCount`。

**更好的方案**：`freeCount` 回到原始的"累计已归还"语义，但加上去重：

```cpp
void CentralCache::performDelayedReturn(size_t index)
{
    delayCounts_[index].store(0, std::memory_order_relaxed);
    lastReturnTimes_[index] = std::chrono::steady_clock::now();

    // 新逻辑：遍历链表，按 Span 分组统计后，直接更新 freeCount
    // 使用 unordered_map 统计每个 Span 当前的 free 块数
    std::unordered_map<SpanTracker*, size_t> spanFreeCounts;

    void* currentBlock = centralFreeList_[index].load(std::memory_order_relaxed);
    while (currentBlock)
    {
        SpanTracker* tracker = getSpanTracker(currentBlock);
        if (tracker)
            spanFreeCounts[tracker]++;
        currentBlock = *reinterpret_cast<void**>(currentBlock);
    }

    for (const auto& [tracker, count] : spanFreeCounts)
    {
        // 问题在这里：ThreadCache 也持有块，但 count 只统计 centralFreeList_ 中的
        // 需要额外机制来得知 ThreadCache 中的块数

        size_t totalFree = count;  // + threadCacheBlocks
        tracker->freeCount.store(totalFree, std::memory_order_release);

        if (totalFree == tracker->blockCount.load(...))
        {
            // 归还整 Span
        }
    }
}
```

**最完善的方案**：让 ThreadCache 在归还时也传递信息，或者 `freeCount` 完全由 `fetchRange` 的递减和 `returnRange`（包括 ThreadCache 持有的）来维护，而不是通过扫描。

## 影响

每个 Span 在第一次 `performDelayedReturn` 后 `freeCount` 就可能超过 `blockCount`，导致该 Span **永远不会被归还给 PageCache**。长期运行的服务器程序会持续堆高内存占用。
