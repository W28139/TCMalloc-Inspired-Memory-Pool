# blockNum == 1 时缺少 SpanTracker，Span 无法回收

## 严重程度

**Bug —— 大块内存（>16KB）永不回收**

## 位置

`src/CentralCache.cpp` `fetchRange` 第 178 行

## 问题描述

```cpp
if (blockNum > 1)
{
    // ... 构建链表 + 创建 SpanTracker ...
}
// blockNum <= 1 → 直接跳过！
// 没有 SpanTracker，没有记录 spanMap_
// 这个 Span 永远不会被 performDelayedReturn 发现
// 永远不会归还 PageCache
```

之前讨论过，`blockNum == 1` 发生在 size > 16KB 时。这种情况下：
- SpanTracker 不创建
- `getSpanTracker` 找不到这个 Span
- Span 永远泄漏（不归还 PageCache，不 munmap）

## 修复

```cpp
if (blockNum > 1)
{
    // ... 现有逻辑 ...
}
else
{
    // blockNum == 1 也需要 SpanTracker
    size_t trackerIndex = spanCount_++;
    if (trackerIndex < spanTrackers_.size())
    {
        spanTrackers_[trackerIndex].spanAddr.store(start, ...);
        spanTrackers_[trackerIndex].numPages.store(numPages, ...);
        spanTrackers_[trackerIndex].blockCount.store(1, ...);
        spanTrackers_[trackerIndex].freeCount.store(0, ...);
        // freeCount=0 因为唯一的一块已返回给 ThreadCache
    }
}
```
