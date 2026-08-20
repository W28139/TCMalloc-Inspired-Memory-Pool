# fetchRange 分支 B：批次内复用 SpanTracker（免 per-block 二分）

## 类型

**性能优化（`14-性能优化方案.md` 第二档第 5 项）**

## 位置

`src/CentralCache.cpp` `fetchRange` 分支 B（逐块更新 `freeCount` 的循环）

## 现状与问题

优化前批次内每块一次 `getSpanTracker`（O(log n) 二分 + 可能 O(64) 线性扫描）：

```cpp
// 优化前：
for (size_t i = 0; i < batchCount; ++i)
{
    SpanTracker* tracker = getSpanTracker(current);   // ← 每块一次二分
    if (tracker)
        tracker->freeCount.fetch_sub(1, std::memory_order_release);
    current = *reinterpret_cast<void**>(current);
}
```

而链表连续块几乎总同属一个 Span（`returnRange` 整段头插、切分时顺序链接），BATCH_SIZE=32 后每批次最多 32 次二分，几乎全是重复查找同一个 Span。

## 实际修复（已应用）

```cpp
// 优化 #23：批次内复用 SpanTracker（免 per-block 二分）
// 链表连续块几乎总同属一个 Span（returnRange 整段插入、切分时顺序链接），
// 缓存当前 tracker，只有块越出当前 Span 区间才重新 getSpanTracker；
// P0-2 修复后 getSpanTracker 不会返回僵尸 tracker，缓存锚定安全。
// 本段持桶锁，同 Span 的归还（同桶锁内置 null）不会与本批次并发。
SpanTracker* cur = nullptr;
void* current = result;
for (size_t i = 0; i < batchCount; ++i)
{
    if (!cur ||
        current < cur->spanAddr.load(std::memory_order_relaxed) ||
        current >= static_cast<char*>(cur->spanAddr.load(std::memory_order_relaxed)) +
                     cur->numPages.load(std::memory_order_relaxed) * PageCache::PAGE_SIZE)
    {
        cur = getSpanTracker(current);   // 典型情况整个批次只进 1 次
    }
    if (cur)
        cur->freeCount.fetch_sub(1, std::memory_order_release);
    current = *reinterpret_cast<void**>(current);
}
```

## 安全分析

| 关注点 | 结论 |
|:---|:---|
| 僵尸 tracker 锚定 | P0-2 修复后 `spanAddr=nullptr` 被 `getSpanTracker` 跳过，返回的一定是有效档案，缓存锚定安全 |
| 归还并发 | 本段持桶锁，同 Span 的归还（`updateSpanFreeCount` 同桶锁内置 null）不并发；不同桶不共享 tracker（一个 Span 只属一个大小类） |
| 区间判断一致性 | `current >= addr && current < addr + numPages*PAGE_SIZE` 与 `getSpanTracker` 的命中条件一致，越出区间即重新查找 |
| `cur == nullptr` 防御 | `getSpanTracker` 返回 nullptr 时 `cur` 保持 nullptr，`if (cur)` 保护，行为与原逻辑等价 |
| 跨 Span 批次 | 链表拼接处（归还段交界）会重新二分一次，退回原逻辑，无额外风险 |

## 预期收益

典型情况二分调用降 32 倍（BATCH_SIZE=32）；多线程场景 +5%~10%。

## 验证

未跑编译与测试（按要求不做验证）。建议：

1. `bin/unit_test` 回归，确认全绿；
2. `bin/perf_test` 多线程场景对比（4 轮平均）。

## 关联

- 方案总纲：`14-性能优化方案.md`（第二档第 5 项）
- **前置条件**：`16-Span归还后SpanTracker未失效.md`（P0-2）
- 前置：`20-BATCH_SIZE提升到32.md`（优化 #20，批次更大、复用收益更高）
