# ThreadCache 归还阈值 256 → 1024

## 类型

**性能优化（`14-性能优化方案.md` 第一档第 4 项）**

## 位置

`src/ThreadCache.cpp`：`shouldReturnToCentralCache`（阈值 256 → 1024）+ 4 处注释同步

## 原理

ThreadCache 本地缓存超过阈值才触发 `returnToCentralCache`（归还 3/4 给 CentralCache）：

- **归还频率降 4 倍**：阈值 256 → 1024，CentralCache 交互次数减少；
- **配合 BATCH_SIZE=32 效果叠加**：每次归还量更大（约 768 块），而 CentralCache 每次批量取走 32 块——跨层交互总量和锁获取次数同步下降。

## 改动内容

```cpp
// 优化 #22：归还阈值 256 → 1024（归还频率降 4 倍，CentralCache 交互减少，
// 配合 BATCH_SIZE=32 每次归还量更大；每线程同大小最多缓存 1055 块，内存可接受）
bool ThreadCache::shouldReturnToCentralCache(size_t index)
{
    size_t threshold = 1024;
    return (freeListSize_[index] > threshold);
}
```

## 风险与边界

| 项 | 分析 |
|:---|:---|
| 每线程内存占用 | 同大小最多缓存 1024 + 31（批量）= 1055 块；如 256B 块 ≈ 270KB/线程，可接受 |
| `freeListSize_` 溢出 | 当前为 `size_t`，无风险；后续若改 `uint16_t`（第 7 项），1055 < 65535 ✓ |
| 归还逻辑 | `returnToCentralCache` 按计数器计算 `actualReturn`（保留 1/4、归还 3/4），与阈值无耦合，无需改动 |

## 验证

未跑编译与测试（按要求不做验证）。建议：

1. `bin/unit_test` 回归，确认全绿；
2. `bin/perf_test` 多线程场景对比（4 轮平均）：预期 +5%~15%。

## 关联

- 方案总纲：`14-性能优化方案.md`（第一档第 4 项）
- 前置：`20-BATCH_SIZE提升到32.md`（优化 #20）、`21-自旋锁yield改mm_pause指数退避.md`（优化 #21）
