# performDelayedReturn 内部使用 std::unordered_map

## 严重程度

**性能 + 安全隐患 —— 持自旋锁期间触发堆分配**

## 位置

`src/CentralCache.cpp` `performDelayedReturn`

## 问题描述

```cpp
void CentralCache::performDelayedReturn(size_t index)
{
    // ↑ 此时正持有 locks_[index] 自旋锁！（由 returnRange 获取）

    std::unordered_map<SpanTracker*, size_t> spanFreeCounts;  // ← 堆分配！

    // ... 遍历链表、插入 map（每次 insert 都可能触发 new） ...

}  // map 析构触发 delete
```

`std::unordered_map` 在构造、insert、析构时都会在堆上分配/释放节点：
1. **耗时长**：堆分配在持锁期间进行，延长自旋锁持有时间
2. **循环依赖**：堆分配走 `malloc`，若被替换为 `MemoryPool::allocate` 则死锁
3. **不可预测**：堆分配可能触发缺页中断，时延波动大

---

## 实际修复（已应用）

### 设计思路

利用 `SpanTracker` 已有的数组存储，增加 `scanCount` 原子字段作为临时计数器。两遍扫描替代 `unordered_map`：

- **第一遍**：遍历 `centralFreeList_`，逐块 `getSpanTracker` → `scanCount.fetch_add(1)`
- **第二遍**：遍历 `trackerArray_`，`scanCount.exchange(0)` 取出计数，>0 则调 `updateSpanFreeCount`

零堆分配，完全在栈上完成。

### 变更 1：`SpanTracker` 增加 `scanCount` 字段

```cpp
struct SpanTracker {
    std::atomic<void*> spanAddr{nullptr};
    std::atomic<size_t> numPages{0};
    std::atomic<size_t> blockCount{0};
    std::atomic<size_t> freeCount{0};
    std::atomic<size_t> scanCount{0};   // ← 新增：临时计数，替代 unordered_map
};
```

### 变更 2：`performDelayedReturn` 重写

**替换前（unordered_map，有堆分配）：**
```cpp
std::unordered_map<SpanTracker*, size_t> spanFreeCounts;  // 堆分配

void* currentBlock = centralFreeList_[index].load(...);
while (currentBlock)
{
    SpanTracker* tracker = getSpanTracker(currentBlock);
    if (tracker) spanFreeCounts[tracker]++;   // insert 可能触发 new
    currentBlock = *reinterpret_cast<void**>(currentBlock);
}

for (const auto& [tracker, newFreeBlocks] : spanFreeCounts)
{
    updateSpanFreeCount(tracker, newFreeBlocks, index);
}
// 析构 spanFreeCounts 触发 delete
```

**替换后（两遍扫描，零堆分配）：**
```cpp
// 第一遍：在 SpanTracker.scanCount 上原子累加
void* currentBlock = centralFreeList_[index].load(...);
while (currentBlock)
{
    SpanTracker* tracker = getSpanTracker(currentBlock);
    if (tracker)
        tracker->scanCount.fetch_add(1, std::memory_order_relaxed);
    currentBlock = *reinterpret_cast<void**>(currentBlock);
}

// 第二遍：遍历 trackerArray_，取出 scanCount 并清零
SpanTracker** array = trackerArray_.load(std::memory_order_acquire);
size_t total = spanCount_.load(std::memory_order_relaxed);

for (size_t i = 0; i < total; ++i)
{
    SpanTracker* tracker = array[i];
    size_t count = tracker->scanCount.exchange(0, std::memory_order_relaxed);
    if (count > 0)
    {
        updateSpanFreeCount(tracker, count, index);
    }
}
```

### 变更 3：移除 `#include <unordered_map>`

头文件中不再需要此依赖。

---

## 压测对比

| 场景 | Fix#6 后 | Fix#7+10 后 | 变化 |
|:---|:---|:---|:---|
| SmallAllocation | 2.082 ms | **1.919 ms** | ↓ **7.8%** |
| MultiThreaded | 6.577 ms | **5.898 ms** | ↓ **10.3%** |
| MixedSizes | 2.285 ms | **1.953 ms** | ↓ **14.5%** |

> Fix#7+10 后 10 轮测试，`bin/perf_test`（WSL2, g++ -O2, C++17）

**结论**：三个场景全面改善。消除自旋锁临界区内的堆分配后，锁持有时间更短、更可预测，多线程下锁竞争进一步缓解（MultiThreaded ↓10%）。MixedSizes 改善最大（↓14.5%）因为混合大小产生更多 Span，`performDelayedReturn` 中 `unordered_map` 的节点分配/释放开销被放大。
