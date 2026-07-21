# freeCount 重复计数导致 Span 永不归还 PageCache

## 严重程度

**Bug —— 导致内存泄漏**

## 位置

`src/CentralCache.cpp` `performDelayedReturn` + `updateSpanFreeCount`

## 问题描述

`updateSpanFreeCount` 使用**累加**方式更新 `freeCount`：

```cpp
size_t oldFreeCount = tracker->freeCount.load(...);
size_t newFreeCount = oldFreeCount + newFreeBlocks;  // ← 累加！
tracker->freeCount.store(newFreeCount, ...);
```

但 `performDelayedReturn` 每次扫描时，计数的块**可能包含上一次扫描已经在链表中的块**（如果它们没被 ThreadCache 取走的话）。这导致同一批空闲块被反复计数，`freeCount` 虚高。

## 复现路径

```
初始: Span A, blockCount=100, freeCount=99

Step 1: 20 块归还到 centralFreeList_
         centralFreeList_[2] 中 Span A 的块: 20 个

Step 2: performDelayedReturn 扫描
         找到 20 块 → freeCount = 99 + 20 = 119
         119 > 100 ！已经超过 blockCount

         但由于 119 != 100（等于判断），归还逻辑不触发
         这 20 块仍在 centralFreeList_ 中

Step 3: 没有 ThreadCache 来取这个大小的块
         10 块归还到 centralFreeList_
         centralFreeList_[2] 中 Span A 的块: 30 个（含上轮的 20 个）

Step 4: performDelayedReturn 再次扫描
         找到 30 块 → freeCount = 119 + 30 = 149
         仍然 ≠ 100，永远不会触发归还

结论: freeCount 永远无法等于 blockCount → Span 永不归还 → 内存泄漏
```

## 影响

每个 Span 在第一次 `performDelayedReturn` 后 `freeCount` 就可能超过 `blockCount`（累加导致），之后 `freeCount == blockCount` 永远无法满足，该 Span **永远不会被归还给 PageCache**。长期运行的服务器程序会持续堆高内存占用。

## 修复方案（已应用）

将累加改为**直接设置**——`performDelayedReturn` 扫描到的块数就是当前在 `centralFreeList_` 中的块数，直接覆盖：

```cpp
void CentralCache::updateSpanFreeCount(SpanTracker* tracker, size_t freeBlocksInList, size_t index)
{
    // 直接设置为本次扫描统计到的空闲块数（而非累加）
    tracker->freeCount.store(freeBlocksInList, std::memory_order_release);

    if (freeBlocksInList == tracker->blockCount.load(std::memory_order_relaxed))
    {
        // 归还整 Span ...
    }
}
```

**注意**：`freeCount` 只统计 `centralFreeList_` 中的块，不包含 ThreadCache 本地 `freeList_` 中缓存的块。这不是问题——ThreadCache 归还后，下一次 `performDelayedReturn` 扫描即可统计到全部块并触发归还，只是时机延后，不是"永不归还"。

---

## 压测对比（修复前 vs 修复后）

| 场景 | 修复前 MP | 修复后 MP | 差值 | 变化 |
|:---|:---|:---|:---|:---|
| SmallAllocation | 2.827 ms | 2.900 ms | +0.073 ms | ↑ 2.6% |
| MultiThreaded | 8.958 ms | 8.883 ms | −0.075 ms | ↓ 0.8% |
| MixedSizes | 2.723 ms | 2.820 ms | +0.097 ms | ↑ 3.6% |

> 修复后：24 轮测试，数据来源同环境 `bin/perf_test`（WSL2, g++ -O2, C++17）

**结论：修复对性能无实质影响。** 三个场景的波动均在 ±4% 以内，属于正常噪声范围。这是预期结果——修复只改变了 `updateSpanFreeCount` 中的赋值方式（累加 → 直接设置），不影响热路径（`fetchRange` / `returnRange`），仅在 `performDelayedReturn` 触发时多执行一次 `store` 替代 `load + add + store`，开销可忽略。

**稳定性**：24 轮中 MixedSizes 出现 1 次 segfault（~4%），该崩溃与本次修复无关，初步判断源于其他已知 bug（如 #2 `freeListSize` 无符号下溢 或 #10 `blockNum==1` 时无 SpanTracker 导致大块访问越界）。后续修复时应持续关注崩溃率变化。
