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

---

## 实际修复（已应用）— 方案 A：动态扩容

### 设计思路

原方案 A 的伪代码用 `std::vector::resize` 扩容，但 `SpanTracker` 含 `std::atomic` 成员，不可拷贝/移动，`vector::resize` 无法编译。直接用 `std::deque` + `std::shared_mutex` 保护读写，但 `getSpanTracker` 是热路径（每次从 `centralFreeList_` 取块都调用），`shared_lock` 开销导致性能减半。

最终采用**无锁读**两层结构：

```
trackerStorage_ (deque<SpanTracker>)   ← 持有对象，deque 保证地址永不改变
        ↑ 指针指向
trackerArray_ (atomic<SpanTracker**>)  ← 扁平指针数组，扩容时原子替换
trackerCount_ (atomic<size_t>)         ← 数组大小，与 trackerArray_ 配对
```

- **读路径**（`getSpanTracker`）：两次 `atomic load` + 原始指针遍历，零锁
- **写路径**（扩容）：`std::mutex` 保护，分配新数组→复制旧指针→构造新对象→原子交换

### 变更 1：头文件 `include/CentralCache.h`

**替换前：**
```cpp
#include <array>
// ...
std::array<SpanTracker, 1024> spanTrackers_;
std::atomic<size_t> spanCount_{0};
```

**替换后：**
```cpp
#include <deque>
// ...
// 两层无锁结构：deque 持有对象，atomic<SpanTracker**> 提供无锁遍历
std::deque<SpanTracker> trackerStorage_;
std::atomic<SpanTracker**> trackerArray_{nullptr};
std::atomic<size_t> trackerCount_{0};
std::atomic<size_t> spanCount_{0};
std::mutex trackerExpandMutex_;                       // 仅保护扩容（极低频）

static const size_t TRACKER_INITIAL_CAPACITY = 1024;  // 初始容量
static const size_t TRACKER_EXPAND_SIZE = 256;        // 每次扩容 256 个槽位
```

### 变更 2：构造函数 `src/CentralCache.cpp`

**替换前：**
```cpp
spanCount_.store(0, std::memory_order_relaxed);
```

**替换后：**
```cpp
spanCount_.store(0, std::memory_order_relaxed);

// 预分配 1024 个 SpanTracker，构建扁平指针数组供 getSpanTracker 无锁遍历
SpanTracker** initialArray = new SpanTracker*[TRACKER_INITIAL_CAPACITY];
for (size_t i = 0; i < TRACKER_INITIAL_CAPACITY; ++i)
{
    trackerStorage_.emplace_back();
    initialArray[i] = &trackerStorage_.back();
}
trackerArray_.store(initialArray, std::memory_order_release);
trackerCount_.store(TRACKER_INITIAL_CAPACITY, std::memory_order_release);
```

### 变更 3：`fetchRange` 中创建 SpanTracker 的逻辑

**替换前：**
```cpp
size_t trackerIndex = spanCount_++;

if (trackerIndex < spanTrackers_.size())  // 超出 1024 后静默跳过！
{
    spanTrackers_[trackerIndex].spanAddr.store(start, ...);
    spanTrackers_[trackerIndex].numPages.store(numPages, ...);
    // ...
}
```

**替换后：**
```cpp
size_t trackerIndex = spanCount_++;

// 容量不足时触发扩容（极低频，仅在活跃 Span 数超过当前容量时）
if (trackerIndex >= trackerCount_.load(std::memory_order_relaxed))
{
    expandTrackerArray(trackerIndex);
}

// 通过无锁数组获取槽位，无需越界检查
SpanTracker** array = trackerArray_.load(std::memory_order_acquire);
SpanTracker* tracker = array[trackerIndex];
tracker->spanAddr.store(start, std::memory_order_release);
tracker->numPages.store(numPages, std::memory_order_release);
tracker->blockCount.store(blockNum, std::memory_order_release);
tracker->freeCount.store(blockNum - 1, std::memory_order_release);
```

### 变更 4：`getSpanTracker` — 无锁遍历

**替换前：**
```cpp
SpanTracker* CentralCache::getSpanTracker(void* blockAddr)
{
    for (size_t i = 0; i < spanCount_.load(...); ++i)
    {
        void* spanAddr = spanTrackers_[i].spanAddr.load(...);
        // ...范围判断...
    }
}
```

**替换后：**
```cpp
SpanTracker* CentralCache::getSpanTracker(void* blockAddr)
{
    // 无锁读：原子加载当前数组指针，直接遍历
    // 扩容时分配新数组 + 原子交换，旧数组保持不变，读者安全
    SpanTracker** array = trackerArray_.load(std::memory_order_acquire);

    for (size_t i = 0; i < spanCount_.load(std::memory_order_relaxed); ++i)
    {
        SpanTracker* tracker = array[i];
        void* spanAddr = tracker->spanAddr.load(std::memory_order_relaxed);
        size_t numPages = tracker->numPages.load(std::memory_order_relaxed);
        // ...范围判断（同前）...
    }
    return nullptr;
}
```

> **为什么无锁安全？** 扩容时分配**新**数组、复制旧指针、原子替换 `trackerArray_`。旧数组本身不被修改，已经拿到旧指针的读者线程可以继续安全遍历。旧数组 ~8KB 泄漏，扩容极低频（1024 个 Span 对应 ~32MB 内存），进程退出时 OS 回收。

### 变更 5：新增 `expandTrackerArray` 方法

```cpp
void CentralCache::expandTrackerArray(size_t requiredIndex)
{
    std::lock_guard<std::mutex> lock(trackerExpandMutex_);  // 串行化扩容

    // 双重检查：其他线程可能已扩容
    if (requiredIndex < trackerCount_.load(std::memory_order_relaxed))
        return;

    size_t oldSize = trackerCount_.load(std::memory_order_relaxed);
    size_t newSize = std::max(oldSize + TRACKER_EXPAND_SIZE, requiredIndex + 1);

    // 1. 分配新指针数组
    SpanTracker** newArray = new SpanTracker*[newSize];

    // 2. 复制旧指针
    SpanTracker** oldArray = trackerArray_.load(std::memory_order_acquire);
    for (size_t i = 0; i < oldSize; ++i)
        newArray[i] = oldArray[i];

    // 3. 构造新的 SpanTracker 对象（deque 保证地址稳定）
    for (size_t i = oldSize; i < newSize; ++i)
    {
        trackerStorage_.emplace_back();
        newArray[i] = &trackerStorage_.back();
    }

    // 4. 原子交换——读路径从此刻起看到新数组
    trackerArray_.store(newArray, std::memory_order_release);
    trackerCount_.store(newSize, std::memory_order_release);

    // 旧数组不释放：可能有读者仍在使用，进程退出时 OS 回收
}
```

### 踩坑记录

第一版直接用 `std::deque` + `std::shared_mutex`，在 `getSpanTracker` 中加 `shared_lock`。结果 `shared_lock`（底层 `pthread_rwlock_rdlock`）在热路径上的开销导致 **SmallAllocation 从 2.7ms 退化到 5.9ms（慢 2 倍）**。改为无锁指针数组后才恢复。

---

## 压测对比

| 场景 | MemoryPool | new/delete | 结论 |
|:---|:---|:---|:---|
| SmallAllocation | 2.611 ms | 1.932 ms | ❌ 慢 35% |
| MultiThreaded | **7.324 ms** | 7.304 ms | ⚖️ 持平（0.2%） |
| MixedSizes | **2.496 ms** | 4.624 ms | ✅ 快 46% |

> Fix#3 后多轮测试取平均值，`bin/perf_test`（WSL2, g++ -O2, C++17）

与 Fix#2 对比：

| 场景 | Fix#2 后 | Fix#3 后 | 变化 |
|:---|:---|:---|:---|
| SmallAllocation | 2.771 ms | 2.611 ms | −5.8% |
| MultiThreaded | 7.621 ms | 7.324 ms | −3.9% |
| MixedSizes | 2.642 ms | 2.496 ms | −5.5% |

**结论：性能无退化，略微改善。** `getSpanTracker` 从 `shared_lock` 改为无锁原子读，消除了热路径上的锁开销。MultiThreaded 与系统 `new/delete` 基本持平（差距仅 0.2%），说明三层架构在多线程场景下已无锁瓶颈。
