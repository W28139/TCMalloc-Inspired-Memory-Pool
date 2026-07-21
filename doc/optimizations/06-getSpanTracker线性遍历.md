# getSpanTracker 线性遍历 O(n)

## 严重程度

**性能优化 —— 每次 fetchRange (分支B) 和 performDelayedReturn 都触发**

## 位置

`src/CentralCache.cpp` `getSpanTracker`

## 问题描述

```cpp
SpanTracker* CentralCache::getSpanTracker(void* blockAddr)
{
    for (size_t i = 0; i < spanCount_.load(...); ++i)  // O(n) 线性扫描
    {
        if (blockAddr >= spanAddr &&
            blockAddr < spanAddr + numPages * PAGE_SIZE)
            return &spanTrackers_[i];
    }
    return nullptr;
}
```

- `fetchRange` 分支 B（从 cache 取块）：批量取 BATCH_SIZE 块，逐块调 `getSpanTracker` 更新 `freeCount`
- `performDelayedReturn`：遍历 `centralFreeList_` 所有块，逐块调 `getSpanTracker` 归组

当活跃 Span 数量增长时，每次查找都线性扫描全部 SpanTracker。

## 复杂度分析

| Span 数量 | 每次查找扫描次数 | 影响 |
|:---|:---|:---|
| 10 | 平均 5 次 | 可忽略 |
| 512 | 平均 256 次 | 明显 |
| 1024+ | 平均 512+ 次 | 每次取块多扫 500+ 个元素 |

---

## 实际修复（已应用）

### 设计思路

采用**懒排序 + 原子替换**，与扩容相同的无锁读模式：

- `sortedCount_`：已按 `spanAddr` 排序的条目数
- `SORT_THRESHOLD = 64`：未排序条目超过 64 个时触发重排
- `getSpanTracker`：排序部分二分查找（O(log n)）+ 未排序尾部线性扫描（≤64 个，O(1)）
- `ensureSorted()`：与 `expandTrackerArray` 相同模式——分配新数组 → 复制 → 排序 → 原子交换

### 变更 1：头文件 `include/CentralCache.h`

```cpp
// 新增
void ensureSorted();                                     // 重排序方法
static const size_t SORT_THRESHOLD = 64;                  // 未排序阈值
std::atomic<size_t> sortedCount_{0};                      // 已排序条目数
```

### 变更 2：`getSpanTracker` — 二分查找 + 线性扫尾

**替换前（O(n)）：**
```cpp
SpanTracker** array = trackerArray_.load(std::memory_order_acquire);
for (size_t i = 0; i < spanCount_.load(...); ++i)   // 全量线性扫描
{
    // ... 地址范围检查 ...
}
```

**替换后（O(log n) + O(1)）：**
```cpp
SpanTracker** array = trackerArray_.load(std::memory_order_acquire);
size_t sorted = sortedCount_.load(std::memory_order_acquire);
size_t total  = spanCount_.load(std::memory_order_relaxed);

// 阶段 1：在已排序 [0, sorted) 上二分查找
size_t left = 0, right = sorted;
while (left < right)
{
    size_t mid = left + (right - left) / 2;
    SpanTracker* t = array[mid];
    void* addr = t->spanAddr.load(std::memory_order_relaxed);

    if (blockAddr < addr)
        right = mid;
    else if (blockAddr >= static_cast<char*>(addr) +
             t->numPages.load(std::memory_order_relaxed) * PageCache::PAGE_SIZE)
        left = mid + 1;
    else
        return t;  // 命中
}

// 阶段 2：未排序尾部 [sorted, total) 线性扫描（最多 64 个）
for (size_t i = sorted; i < total; ++i)
{
    // ... 同前地址范围检查 ...
}
return nullptr;
```

> 二分查找正确性保证：Spans 来自 PageCache 的非重叠内存区域，按 `spanAddr` 排序后严格有序。`blockAddr` 落在且仅落在一个 Span 的 `[spanAddr, spanAddr+numPages*PAGE_SIZE)` 范围内。

### 变更 3：`expandTrackerArray` — 重建时全排序

```cpp
// 扩容重建新数组后，排序（与替换前比新增 3 行）
std::sort(newArray, newArray + newSize,
    [](SpanTracker* a, SpanTracker* b) {
        return a->spanAddr.load(std::memory_order_relaxed) <
               b->spanAddr.load(std::memory_order_relaxed);
    });

trackerArray_.store(newArray, std::memory_order_release);
trackerCount_.store(newSize, std::memory_order_release);
sortedCount_.store(newSize, std::memory_order_release);   // 新数组全排序
```

### 变更 4：`fetchRange` 分支 A — 触发重排

```cpp
// SpanTracker 初始化完成后
tracker->freeCount.store(blockNum - batchCount, std::memory_order_release);

// 未排序条目超阈值 → 触发重排
size_t total = spanCount_.load(std::memory_order_relaxed);
size_t sorted = sortedCount_.load(std::memory_order_relaxed);
if (total > sorted && (total - sorted) > SORT_THRESHOLD)
{
    ensureSorted();
}
```

### 变更 5：新增 `ensureSorted()` 方法

```cpp
void CentralCache::ensureSorted()
{
    // 双重检查
    if (spanCount_.load() - sortedCount_.load() <= SORT_THRESHOLD) return;

    std::lock_guard<std::mutex> lock(trackerExpandMutex_);
    // 再次双重检查...

    size_t total = spanCount_.load(), arraySize = trackerCount_.load();
    SpanTracker** newArray = new SpanTracker*[arraySize];

    // 复制所有指针
    SpanTracker** oldArray = trackerArray_.load(std::memory_order_acquire);
    for (size_t i = 0; i < total; ++i) newArray[i] = oldArray[i];
    for (size_t i = total; i < arraySize; ++i) newArray[i] = oldArray[i];

    // 对已使用部分排序
    std::sort(newArray, newArray + total,
        [](SpanTracker* a, SpanTracker* b) {
            return a->spanAddr.load() < b->spanAddr.load();
        });

    // 原子交换
    trackerArray_.store(newArray, std::memory_order_release);
    sortedCount_.store(total, std::memory_order_release);
}
```

---

## 压测对比

| 场景 | Fix#5 后 | Fix#6 后 | 变化 |
|:---|:---|:---|:---|
| SmallAllocation | 2.657 ms | **2.082 ms** | ↓ **21.6%** |
| MultiThreaded | 6.392 ms | 6.577 ms | +2.9% |
| MixedSizes | 2.479 ms | 2.285 ms | −7.8% |

| 场景 | MemoryPool | new/delete | 结论 |
|:---|:---|:---|:---|
| SmallAllocation | **2.082 ms** | 2.025 ms | ⚖️ 仅慢 **3%** |
| MultiThreaded | **6.577 ms** | 7.741 ms | ✅ 快 **15%** |
| MixedSizes | **2.285 ms** | 4.858 ms | ✅ 快 **53%** |

> Fix#6 后 10 轮测试，`bin/perf_test`（WSL2, g++ -O2, C++17）

**结论**：SmallAllocation 改善最显著（↓21.6%），从慢 35% 缩小到仅慢 3%，几乎追平系统 malloc。原因：SmallAllocation 的 6 种固定大小产生 ~6 个 Span，每次 `fetchRange` 分支 B 取块都要调 `getSpanTracker`，O(log n) 替代 O(n) 的收益被高频调用放大。MultiThreaded 持平——多线程下锁竞争掩盖了查找开销的改善。
