# blockNum == 1 时缺少 SpanTracker，大块永不回收

## 严重程度

**Bug —— 大块内存（>16KB）永不回收，每个泄漏 32KB+**

## 位置

`src/CentralCache.cpp` `fetchRange` 分支 A，`blockNum` 计算后

## 问题描述

当请求大小 > 16KB 时，8 页 Span（32KB）只能切分成 1 块：

```
请求 size = 20KB → blockNum = 32KB / 20KB = 1
请求 size = 24KB → blockNum = 32KB / 24KB = 1
```

原代码直接跳过 SpanTracker 创建：

```cpp
if (blockNum > 1)
{
    // 构建链表 + 创建 SpanTracker ...
}
// blockNum <= 1 → 跳过！
// SpanTracker 不创建 → getSpanTracker 永远找不到 →
// updateSpanFreeCount 永远不触发 → Span 永不归还 PageCache
```

每个未追踪的大块 Span 泄漏 32KB（8 页）。在频繁分配 16~32KB 对象的场景下（如网络缓冲区、序列化缓冲区），几个小时内就会泄漏数十 MB。

## 触发条件

- 分配大小 ∈ (16KB, 32KB] → `blockNum = 1`
- CentralCache 为空时需要新 Span → 走分支 A → SpanTracker 跳过 → 泄漏

---

## 实际修复（已应用）

`blockNum == 1` 时仍然创建 SpanTracker，`freeCount = 0`（唯一一块已返回给调用者）。

### 变更：`fetchRange` 分支 A

**替换前：**
```cpp
if (blockNum > 1)
{
    // ... 构建链表 + 创建 SpanTracker ...
}
// blockNum <= 1：直接跳过，无 SpanTracker → 泄漏
```

**替换后：**
```cpp
if (blockNum > 1)
{
    // ... 构建链表 + 创建 SpanTracker（原有逻辑不变）...
}
else
{
    // blockNum == 1：大对象场景，无需链表但必须创建 SpanTracker
    size_t trackerIndex = spanCount_++;

    if (trackerIndex >= trackerCount_.load(std::memory_order_relaxed))
        expandTrackerArray(trackerIndex);

    SpanTracker** array = trackerArray_.load(std::memory_order_acquire);
    SpanTracker* tracker = array[trackerIndex];

    tracker->spanAddr.store(start, std::memory_order_release);
    tracker->numPages.store(numPages, std::memory_order_release);
    tracker->blockCount.store(1, std::memory_order_release);
    // freeCount = 0：唯一一块已返回，Span 中无空闲块
    // 当调用者归还该块 → returnRange → performDelayedReturn →
    // scanCount=1 → freeCount 设为 1 → blockCount==1 → 归还 PageCache ✓
    tracker->freeCount.store(0, std::memory_order_release);

    // 触发重排序检查（与其他分支一致）
    size_t total = spanCount_.load(std::memory_order_relaxed);
    size_t sorted = sortedCount_.load(std::memory_order_relaxed);
    if (total > sorted && (total - sorted) > SORT_THRESHOLD)
        ensureSorted();
}
```

### 归还路径验证

```
1. allocate(24KB) → fetchRange → blockNum=1 → SpanTracker(freeCount=0) → 返回块
2. deallocate(块) → returnToCentralCache → returnRange → centralFreeList_ 有这个块
3. performDelayedReturn → getSpanTracker(块) → 找到 SpanTracker → scanCount=1
4. updateSpanFreeCount → freeCount=1 → blockCount==1 → 归还 PageCache ✓
```

---

## 压测对比

本修复与 #07 同时应用，压测数据见 `07-performDelayedReturn中使用unordered_map.md`。大块分配场景（16~32KB）此前会静默泄漏，修复后正确回收。
