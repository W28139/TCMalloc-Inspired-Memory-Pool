# Span 归还后 SpanTracker 未失效（僵尸 tracker），地址复用时二分误命中 → 潜在 UAF

## 严重程度

**致命问题（P0-2，来自 `13-review.md`）—— 稳定工作负载下必然发生，Use-After-Free / 双归还**

## 位置

`src/CentralCache.cpp` `updateSpanFreeCount`（修复前 :463，归还 PageCache 后未清 `spanAddr`）

## 问题描述

Span 全空闲归还 PageCache 时，**只调用了 `PageCache::deallocateSpan`，没有清除该 tracker 的 `spanAddr`**：

```cpp
// 修复前（有缺陷）：归还后 tracker 的 spanAddr 仍指向已归还的地址 X
PageCache::getInstance().deallocateSpan(spanAddr, numPages);
// ← 缺少：tracker->spanAddr.store(nullptr)
```

而 PageCache 采用 Best-Fit（`PageCache.cpp:62`），**同一大小的 Span 几乎必然复用刚归还的地址**。于是：

```
① 分配 Span X         → tracker A {spanAddr=X, blockCount=1024}
② 全部块空闲         → updateSpanFreeCount(A) 判定全空闲
③ 归还 X 给 PageCache → freeSpans_ 缓存了 X   ← tracker A 未失效！spanAddr 还是 X
④ 再次申请同大小     → Best-Fit 命中 X，重新分配出去
                       新 tracker B {spanAddr=X, blockCount=1024}
⑤ 数组里 A、B 的地址区间完全重叠
⑥ getSpanTracker(X 内某块) → 命中 A 还是 B 取决于排序相对位置（std::sort 对相同键不稳定）≈ 50%
```

**后果**：

1. **命中僵尸 A 且 `count == A.blockCount`**（同大小 Span 块数相同，几乎必然成立）：`updateSpanFreeCount(A)` 判定"A 全空闲" → **把地址 X 再次归还 PageCache**——而 X 是 B 正在使用的内存 → 被 munmap 或并入 `freeSpans_` 再次分配 → **悬垂指针 / Use-After-Free / 双归还**（段错误或静默数据损坏）；
2. 命中僵尸 A 而 count 不等：B 的 `freeCount` 永不更新 → B 永不归还 → 泄漏。

> 与 P0-1 不同，此处影响的是 `centralFreeList_` 之外的数据结构（`trackerArray_` 的档案），归还时链表摘除逻辑本身是正确执行的。

---

## 实际修复（已应用）

### 设计思路

归还 PageCache 后，在桶锁内使 tracker 失效：`spanAddr` 置 `nullptr`（`freeCount` 归 0）。置 null 后 `getSpanTracker` 对空槽的两种边界比较恰好是"跳过"语义（`blockAddr < nullptr` 为 false → 不右移；`blockAddr >= nullptr + 0` 为 true → 左移），二分与线性扫描均不会命中，天然免疫。地址 X 的新档案 B 成为唯一持有者，双归还不再可能。

### 变更：`updateSpanFreeCount` 归还后置空

**替换前（有缺陷）：**
```cpp
// 归还 Span 给 PageCache（可能被合并、缓存或 munmap）
PageCache::getInstance().deallocateSpan(spanAddr, numPages);
```

**替换后（已应用）：**
```cpp
// 归还 Span 给 PageCache（可能被合并、缓存或 munmap）
PageCache::getInstance().deallocateSpan(spanAddr, numPages);

// 修复 P0-2：归还后使 tracker 失效，防止地址复用时二分误命中僵尸档案。
// PageCache Best-Fit 几乎必然复用刚归还的地址 X；若 tracker 仍持有 X，
// 与新 tracker 地址区间重叠，二分约 50% 命中旧档案 → 双归还正在使用的内存 → UAF。
// 置 null 后 getSpanTracker 的边界比较（blockAddr < addr 为 false、
// blockAddr >= addr + 0 为 true）会安全跳过空槽，天然免疫。
tracker->spanAddr.store(nullptr, std::memory_order_release);
tracker->freeCount.store(0, std::memory_order_release);
```

### 修复后的行为

- `getSpanTracker` 永远命中有效档案：计数（`scanCount`/`freeCount`）始终落在正确 tracker 上，Span 正常归还，无泄漏；
- 排序/二分/`sortedCount_` 均无需改动：置 null 不破坏"前缀有序"的二分前提（nullptr 与有效地址、nullptr 与 nullptr 之间均满足序关系）；空槽排最前时二分跳过，行为一致；
- 僵尸条目仍占用数组槽位（每归还一个 Span 约 8B 指针 + 40B 对象，单调增长）——安全但占位，经评估影响可接受（2026-08-18），不实施回收。

---

## 验证

未跑编译与测试（按要求不做验证）。建议：

1. `bin/unit_test` 回归，确认全绿；
2. 稳态循环：同大小分配 → 全归还 → 再分配，运行数分钟观测 RSS 不增长、无崩溃（能覆盖本条与 P0-1 的修复路径）；
3. 白盒断言：`getSpanTracker` 对每个已分配块必须命中且唯一命中（能抓僵尸误命中回归）。

## 关联

- 完整 review：`13-review.md`（P0-2）
- 性能方案：`14-性能优化方案.md`——本修复是第 5 项优化（fetchRange 分支 B 批次内复用 tracker，多线程 +5%~10%）的**前置条件**
- 前置修复：`15-expandTrackerArray扩容排序覆盖活跃SpanTracker.md`（P0-1）
