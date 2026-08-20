# 并发下 trackerArray_ 与 sortedCount_ / spanCount_ 的组合读取窗口

## 严重程度

**严重问题（P1-2，来自 `13-review.md`）—— 低概率但真实：二分漏查导致 Span 永不归还（泄漏）、数组越界遍历/排序（崩溃）**

## 位置

`src/CentralCache.cpp`：`getSpanTracker`（读）、`performDelayedReturn`（读）、`ensureSorted`（写 + 排序）、`expandTrackerArray`（写）

## 问题描述

不同桶锁的线程并发读写 tracker 数组（A 持桶锁 X 执行扩容/重排，B 持桶锁 Y 执行查找/遍历）。写方与读方的多步读取无法原子配对，存在三种组合窗口：

**窗口 1：新数组 + 旧 sorted（review 已分析，无害）**
二分在 `[0, 旧sorted)` 子区间查找（新数组 `[0, 新sorted)` 已排序，旧sorted ≤ 新sorted），miss 时线性扫描 `[旧sorted, total)` 范围扩大，**能兜底找到**。✓

**窗口 2：旧数组 + 新 sorted（review 未覆盖，有害）**
写序上 `store(array)` 与 `store(sortedCount)` 之间有间隙，读者先读到旧数组、后读到新 sorted：

- 二分在旧数组 `[0, 新sorted)` 上查找，其中 `[旧sorted, 新sorted)` 是**未排序区** → miss；
- 线性扫描从 `新sorted` 开始 → 恰好跳过漏查区域 → `getSpanTracker` 返回 nullptr；
- 后果：`performDelayedReturn` 第一遍 `scanCount` 不累加 → 该 Span 的 `freeCount` 永不更新 → **Span 永不归还 PageCache，永久泄漏**。

> 触发前提：`ensureSorted` 被触发（`total - sorted > 64`）时目标 tracker 恰在新增排序区 `[旧sorted, 新sorted)`。长期运行（数十亿次 `getSpanTracker`）必然命中。

**窗口 3：旧数组 + 新 total（`spanCount_` 先于扩容递增）**
`fetchRange` 先 `spanCount_++`（fetch_add），后 `expandTrackerArray`（store 新数组）：

- `performDelayedReturn` 遍历 `[0, total)` 读"旧数组 + 新 total" → **越界读**（新 total > 旧容量）→ 解引用垃圾指针 → 崩溃/UB；
- `getSpanTracker` 阶段 2 线性扫描 `[sorted, total)` 同样越界；
- `ensureSorted` 持锁后 `total` 可能暂时 > `arraySize`（其他线程已 fetch_add 未扩容）→ `std::sort(newArray, newArray + total)` **堆越界**。

## 修复方案（已应用）

### 1. 写序契约（不变，注释固定）

- `trackerArray_` 的 store **先于** `trackerCount_` / `sortedCount_` 的 store（`expandTrackerArray` / `ensureSorted` 内已是此序，补注释固化）；
- 旧数组永不复用、指针只前进（无 ABA）。

### 2. 读者双读校验（seqlock 模式）

`getSpanTracker` / `performDelayedReturn` 中，两次 load `trackerArray_` 一致才使用读到的 `capacity`、`sorted`：

```cpp
SpanTracker** array;
size_t capacity;
size_t sorted;
for (;;)
{
    array = trackerArray_.load(std::memory_order_acquire);
    capacity = trackerCount_.load(std::memory_order_acquire);
    sorted = sortedCount_.load(std::memory_order_acquire);
    if (trackerArray_.load(std::memory_order_acquire) == array)
        break;
}
```

正确性：两次 array 一致 ⇒ 期间无扩容/重排（写方先 store array 后 store capacity/sorted，同一临界区）⇒ capacity、sorted 必与 array 配对 ⇒ 窗口 2 被消除。窗口 1（新数组 + 旧 sorted）仍可能但无害。

### 3. 上界截断 `min(total, capacity)`

`spanCount_` 先于扩容递增，窗口 3 无法仅靠读序消除；所有遍历/扫描/排序上界取 `min(total, capacity)`：

- `getSpanTracker`：二分 `right = min(sorted, scanEnd)`、线性扫描 `[sorted, scanEnd)`；
- `performDelayedReturn`：遍历 `[0, scanEnd)`；
- `ensureSorted`：`used = min(total, arraySize)`，复制与排序范围均为 `[0, used)`，`sortedCount_.store(used)`。

刚创建的 Span（total 增、数组未换）延迟一轮被扫描，无害。

### 4. 顺带修复 P2-10

`ensureSorted` 两个复制循环合并为 `for (i = 0; i < arraySize; ++i) newArray[i] = oldArray[i];`（持锁期间 array 与 trackerCount_ 配对一致，oldArray 容量 == arraySize，无越界）。

## 验证

未跑编译与测试（按要求不做验证）。建议：

1. `bin/unit_test` 回归，确认全绿；
2. 扩容路径压力测试（2000+ Span 分配触发多次扩容）+ 多线程归还并发，运行数分钟确认无崩溃、无泄漏；
3. 白盒断言：`getSpanTracker` 对每个已分配块必须命中且唯一命中。

## 关联

- 完整 review：`13-review.md`（P1-2、P2-10）
- 前置修复：`15-expandTrackerArray扩容排序覆盖活跃SpanTracker.md`（P0-1）、`16-Span归还后SpanTracker未失效.md`（P0-2）
