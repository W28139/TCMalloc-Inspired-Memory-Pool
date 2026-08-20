# expandTrackerArray 扩容时排序整个数组，覆盖活跃 SpanTracker

## 严重程度

**致命问题（P0-1，来自 `13-review.md`）—— 长期运行内存泄漏 + 二分查找失效**

## 位置

`src/CentralCache.cpp` `expandTrackerArray`（修复前 :576-586）

## 问题描述

SpanTracker 数组存在两套互相矛盾的索引语义：

- **创建序**：`trackerIndex = spanCount_++`，第 N 个创建的 Span 用数组下标 N（`fetchRange` 分支 A）；
- **地址序**：`expandTrackerArray` / `ensureSorted` 按 `spanAddr` 排序，供二分查找。

`expandTrackerArray` 扩容时对**整个数组（含刚 new 出来的空槽）**执行排序：

```cpp
// 修复前（有缺陷）：
std::sort(newArray, newArray + newSize,          // ← 排序范围含空槽（spanAddr=nullptr）
    [](SpanTracker* a, SpanTracker* b) {
        return a->spanAddr.load(std::memory_order_relaxed) <
               b->spanAddr.load(std::memory_order_relaxed);
    });
// ...
sortedCount_.store(newSize, std::memory_order_release);   // ← 含空槽的 newSize
```

排序后空槽（`spanAddr=nullptr`）全部排到数组**最前面**，活跃 tracker 全部排在后面。此时 `array[trackerIndex]`（创建序下标）落在**活跃区中间**，新 Span 的数据（`spanAddr`/`numPages`/`blockCount`/`freeCount`）会覆盖一个正在使用的 SpanTracker。

### 触发条件与后果

1. **触发**：`spanCount_` 首次达到 1024（约 32MB 活跃内存）触发扩容后，**此后每次分配新 Span 都覆盖一个活跃 tracker**，直到数组再满、再次扩容，雪崩式破坏；
2. **泄漏**：被覆盖的旧 Span 在数组中"消失"，`getSpanTracker` 永远找不到它 → `performDelayedReturn` 无法判定其全空闲 → **该 Span 永不归还 PageCache，永久内存泄漏**；
3. **二分失效**：`sortedCount_`（含空槽数）> `spanCount_`（实际使用数），数组出现"空洞"；且阶段 2 线性扫描 `for (i = sorted; i < total; ++i)` 因 `sorted > total` **永不执行**，二分一旦漏查就彻底查不到（P1-1 的放大器）。

> 单元测试全部通过是因为测试规模 < 32MB 活跃内存，触发不了扩容路径，缺陷被掩盖。

---

## 实际修复（已应用，方案 A——最小改动）

### 设计思路

扩容时**不要排序**：新空槽一律追加在数组尾部，与创建序索引一致；`sortedCount_` 保持旧值。排序只由 `ensureSorted` 执行，它只排已使用的 `[0, spanCount_)` 部分，本来就是正确的。

### 变更：`expandTrackerArray` 删除排序，`sortedCount_` 不更新

**替换前（有缺陷）：**
```cpp
// 按 spanAddr 排序，支持二分查找（修复 #06）
std::sort(newArray, newArray + newSize,
    [](SpanTracker* a, SpanTracker* b) {
        return a->spanAddr.load(std::memory_order_relaxed) <
               b->spanAddr.load(std::memory_order_relaxed);
    });

// 原子交换
trackerArray_.store(newArray, std::memory_order_release);
trackerCount_.store(newSize, std::memory_order_release);
sortedCount_.store(newSize, std::memory_order_release);
```

**替换后（已应用）：**
```cpp
// 原子交换
trackerArray_.store(newArray, std::memory_order_release);
trackerCount_.store(newSize, std::memory_order_release);
// sortedCount_ 不更新：新槽位尚未排序，仍属于"未排序尾部"
```

### 修复后的不变式

- 数组尾部永远是未使用的空槽（`spanAddr=nullptr`），`array[trackerIndex]` 始终指向真正的空槽；
- `sortedCount_ <= spanCount_` 恒成立（P1-1 随之消失）；
- 二分前提恢复：排序区间 `[0, sortedCount_)` 内按地址升序、区间两两不重叠；
- 扩容后新增的条目落入"未排序尾部"，由 `fetchRange` 已有的懒排序触发器（`total - sorted > SORT_THRESHOLD` → `ensureSorted`）在阈值处统一排序。

---

## 验证

未跑编译与测试（本次修复按要求不做验证）。建议：

1. `bin/unit_test` 回归，确认全绿；
2. 稳态循环：同大小分配 → 全归还 → 再分配，运行数分钟观测 RSS 不增长；
3. 2000+ 个 Span 分配压力测试（`spanCount_ > 1024` 触发扩容路径）。

## 关联

- 完整 review：`13-review.md`（P0-1 / P1-1）
- 性能方案：`14-性能优化方案.md`（先修 P0 再做第一档优化）
- 遗留：**P0-2（归还后 SpanTracker 未失效 → 地址复用时二分误命中僵尸 tracker → 潜在 UAF）尚未修复**，见 `13-review.md` 第五节。
