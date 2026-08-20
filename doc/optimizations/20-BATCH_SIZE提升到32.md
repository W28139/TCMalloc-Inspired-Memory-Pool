# BATCH_SIZE 8 → 32（批量传输粒度提升）

## 类型

**性能优化（`14-性能优化方案.md` 第一档第 2 项）**

## 位置

- `include/CentralCache.h:81`：`static const size_t BATCH_SIZE = 8` → `32`
- 注释同步：`include/ThreadCache.h:18`、`src/ThreadCache.cpp:121`、`:125`（`BATCH_SIZE(8)` → `(32)`）

## 原理

`fetchRange` 每次跨层调用：取桶锁 → 批量摘 `BATCH_SIZE` 块 → 放桶锁。批量粒度 8 → 32：

- **跨层调用次数降 4 倍**：ThreadCache 缺货时的 `fetchFromCentralCache` 触发频率不变，但每次拿回 32 块（本地缓存 31 块），后续 31 次分配命中本地 `freeList_`，无需跨层；
- **桶锁获取次数降 4 倍**：每次 `fetchRange` 的 `test_and_set` / `clear` 只发生一次，多线程下锁竞争显著降低；
- tcmalloc 批量 32 是业界经验值。

## 改动内容

```cpp
// 优化 #20：BATCH_SIZE 8 → 32（tcmalloc 经验值），跨层调用与桶锁获取次数降 4 倍
static const size_t BATCH_SIZE = 32;                  // fetchRange 批量传输 32 块
```

无需改其他代码：`fetchRange` 分支 A/B 的批量循环（`batchCount < BATCH_SIZE`）和 ThreadCache 的链表计数（`batchNum`）都按上限工作，无硬编码 8 的依赖。

## 风险与边界

| 项 | 分析 |
|:---|:---|
| ThreadCache 本地缓存上限 | 归还阈值 256 + 批量 31 = 最多 287 块/大小类，内存占用略增（如 256B 块 ≈ 73KB/线程），可接受 |
| `freeListSize_` 溢出 | 当前为 `size_t`，无风险；后续若改 `uint16_t`（`14-性能优化方案.md` 第 7 项），287 < 65535 ✓ |
| 分支 A（新 Span） | `batchCount` 初始化 1、循环受 `blockNum` 约束，`blockNum < 32` 时取更少块，逻辑不变 |

## 验证

未跑编译与测试（按要求不做验证）。建议：

1. `bin/unit_test` 回归，确认全绿；
2. `bin/perf_test` 对比（4 轮平均）：多线程场景预期 +15%~30%，单线程持平或微胜。

## 关联

- 方案总纲：`14-性能优化方案.md`（第一档第 2 项）
- 正确性前置：P0-1/P0-2/P1-1~P1-4 已全部修复（`15-19` 号日志）
