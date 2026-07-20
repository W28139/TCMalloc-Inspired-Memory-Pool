# performDelayedReturn 内部使用 std::unordered_map

## 严重程度

**性能 + 安全隐患 —— 持自旋锁期间触发了堆分配**

## 位置

`src/CentralCache.cpp` `performDelayedReturn` 第 471 行

## 问题描述

```cpp
void CentralCache::performDelayedReturn(size_t index)
{
    // ↑ 此时正持有 locks_[index] 自旋锁！（由 returnRange 获取）

    std::unordered_map<SpanTracker*, size_t> spanFreeCounts;  // ← 堆分配！

    // ... 遍历链表、插入 map（每次 insert 都可能触发 new） ...

}  // map 析构（可能触发 delete）
```

`std::unordered_map` 在构造、insert、析构时都会在堆上分配/释放节点。这些操作：
1. **耗时长**：堆分配在持锁期间进行，延长了自旋锁的持有时间
2. **循环依赖**：堆分配走 `malloc`，如果 `malloc` 被替换为 `MemoryPool::allocate`，可能死锁
3. **不可预测**：堆分配可能触发缺页中断，时延波动大

## 修复方案

### 方案 A：使用栈上的小哈希表

每个 SpanTracker 已经在 `spanTrackers_[1024]` 数组中。可以为每个 SpanTracker 增加一个**线程局部的计数**字段，遍历时直接累加：

```cpp
struct SpanTracker {
    std::atomic<void*> spanAddr{nullptr};
    std::atomic<size_t> numPages{0};
    std::atomic<size_t> blockCount{0};
    std::atomic<size_t> freeCount{0};
    std::atomic<size_t> scanCount{0};    // ← 新增：临时计数字段
};

void CentralCache::performDelayedReturn(size_t index)
{
    // 不用 unordered_map 了！

    void* currentBlock = centralFreeList_[index].load(...);
    while (currentBlock)
    {
        SpanTracker* tracker = getSpanTracker(currentBlock);
        if (tracker)
            tracker->scanCount.fetch_add(1, std::memory_order_relaxed);
        currentBlock = *reinterpret_cast<void**>(currentBlock);
    }

    // 第二遍扫描 spanTrackers_，处理有 scanCount > 0 的
    for (size_t i = 0; i < spanCount_.load(...); ++i)
    {
        size_t count = spanTrackers_[i].scanCount.exchange(0, ...);
        if (count > 0)
        {
            updateSpanFreeCount(&spanTrackers_[i], count, index);
        }
    }
}
```

优点：零堆分配，栈上完成。

### 方案 B：固定大小的 C 数组替代 unordered_map

```cpp
// 最多 1024 个 Span，可以用 pair 数组
std::pair<SpanTracker*, size_t> counts[1024];
size_t uniqueSpans = 0;

// 遍历时线性查找/插入
while (currentBlock) {
    SpanTracker* t = getSpanTracker(currentBlock);
    // 在 counts[0..uniqueSpans-1] 中查找 t，找不到则追加
    // ...
}
```

## 收益

- 消除自旋锁临界区内的堆分配
- 避免 `malloc` 循环依赖风险
- 锁持有时间更可预测
