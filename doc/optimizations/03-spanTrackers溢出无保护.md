# spanTrackers_ 数组溢出无保护

## 严重程度

**Bug —— 超出 1024 个 Span 后静默内存泄漏**

## 位置

`src/CentralCache.cpp` `fetchRange` 第 232-245 行

## 问题描述

```cpp
size_t trackerIndex = spanCount_++;   // 原子递增，永不停歇

if (trackerIndex < spanTrackers_.size())  // 超出 1024 后跳过
{
    spanTrackers_[trackerIndex].spanAddr.store(...);
    spanTrackers_[trackerIndex].numPages.store(...);
    // ...
}
// trackerIndex >= 1024 → 静默跳过，SpanTracker 不创建！
```

当分配的 Span 数量超过 1024 后：
- 新的 Span 没有 SpanTracker 记录
- `getSpanTracker` 永远找不到这些 Span
- 这些 Span 的空闲块**永远不会被检测到**，**永远无法归还 PageCache**
- 每个未追踪的 Span 泄漏 32KB（8 页）或更多

## 触发条件

每个大小类别至少消耗 1 个 Span（CentralCache 为空时从 PageCache 拿）。有 32768 个大小类别，但实际触发的只有被使用到的那些。在一个长期运行的多线程程序中，1024 个 Span 可能几个小时内就用完。

## 修复方案

### 方案 A：动态扩容（推荐）

```cpp
std::vector<SpanTracker> spanTrackers_;  // 改为动态数组

// fetchRange 中：
size_t trackerIndex = spanCount_++;
if (trackerIndex >= spanTrackers_.size())
{
    spanTrackers_.resize(spanTrackers_.size() + 256);  // 批量扩容
}
spanTrackers_[trackerIndex].spanAddr.store(...);
```

### 方案 B：回收已归还 Span 的槽位

归还整 Span 给 PageCache 后，将该 SpanTracker 槽位标记为"可复用"，用空闲链表管理槽位：

```cpp
struct SpanTracker {
    std::atomic<void*> spanAddr{nullptr};
    std::atomic<size_t> numPages{0};
    std::atomic<size_t> blockCount{0};
    std::atomic<size_t> freeCount{0};
    std::atomic<size_t> nextFree{0};     // 空闲槽位链表
    std::atomic<bool> inUse{false};       // 是否在用
};
```

缺点：增加复杂度，O(n) 查找变为需要跳过已释放的槽位。

### 方案 C：提高硬上限并加日志

```cpp
static constexpr size_t MAX_SPANS = 8192;  // 从 1024 提升

if (trackerIndex >= spanTrackers_.size())
{
    // 至少打个日志
    fprintf(stderr, "[CentralCache] WARNING: spanTrackers_ overflow at %zu\n", trackerIndex);
}
```
