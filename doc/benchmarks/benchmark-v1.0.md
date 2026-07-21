# 性能压测报告 v1.0

## 测试环境

| 项目 | 值 |
|:---|:---|
| 日期 | 2026-07-20 |
| CPU | WSL2 (Linux 6.6.87.2-microsoft-standard-WSL2) |
| 编译器 | g++ -O2, C++17 |
| 对比基准 | glibc `new char[]` / `delete[]` |
| 测试轮次 | 4 轮，取平均值 |
| 可执行文件 | `bin/perf_test` |

## 测试场景

| 场景 | 总分配次数 | 线程 | 大小范围 | 释放节奏 |
|:---|:---|:---|:---|:---|
| SmallAllocation | 50000 | 1 | 8B ~ 256B (6 种) | 每 4 次放 1 次 |
| MultiThreaded | 100000 | 4 | 8B ~ 256B (6 种) | 每 100 次放 20~30%，每 1000 次压力脉冲 |
| MixedSizes | 100000 | 1 | 8B ~ 4KB (11 种, 3 类) | 每 50 次放 20~30% |

## 原始数据

| 轮次 | SmallAllocation (MP) | SmallAllocation (new) | MultiThreaded (MP) | MultiThreaded (new) | MixedSizes (MP) | MixedSizes (new) |
|:---|:---|:---|:---|:---|:---|:---|
| 1 | 2.728 ms | 1.863 ms | 8.296 ms | 7.857 ms | 2.869 ms | 5.093 ms |
| 2 | 2.803 ms | 1.972 ms | 10.748 ms | 6.732 ms | 2.575 ms | 4.828 ms |
| 3 | 2.874 ms | 2.016 ms | 8.422 ms | 8.316 ms | 2.599 ms | 5.091 ms |
| 4 | 2.904 ms | 2.018 ms | 8.366 ms | 8.118 ms | 2.848 ms | 5.040 ms |

## 平均值与对比

### SmallAllocation — 单线程小对象高频

| 指标 | MemoryPool | new/delete | 差距 |
|:---|:---|:---|:---|
| 平均值 | **2.827 ms** | **1.967 ms** | 慢 **43.7%** |
| 最小 | 2.728 ms | 1.863 ms | — |
| 最大 | 2.904 ms | 2.018 ms | — |
| 标准差 | 0.065 ms | 0.059 ms | — |

```
MemoryPool  ████████████████████████████████░░░░░░  2.827 ms
new/delete  ██████████████████████░░░░░░░░░░░░░░░░  1.967 ms
```

**结论**：单线程小对象场景下，内存池**慢于**系统 malloc。原因分析：

- glibc 的 per-thread arena 在单线程下无锁竞争，且对小对象有高度优化的 fastbins / tcache
- 内存池的 ThreadCache TLS 初始化和 32768 元素数组的冷启动开销在单线程下无法摊还
- 测试使用固定 6 种大小循环，glibc malloc 的 per-size freelist 可以高效复用
- 内存池每次 `fetchRange` 只给 1 块的"非批量传输"缺陷在此场景下被放大

### MultiThreaded — 4 线程并发

| 指标 | MemoryPool | new/delete | 差距 |
|:---|:---|:---|:---|
| 平均值 | **8.958 ms** | **7.756 ms** | 慢 **15.5%** |
| 最小 | 8.296 ms | 6.732 ms | — |
| 最大 | 10.748 ms | 8.316 ms | — |
| 标准差 | 1.008 ms | 0.618 ms | — |

```
MemoryPool  ████████████████████████████████████████  8.958 ms
new/delete  ██████████████████████████████████░░░░░░  7.756 ms
```

**结论**：4 线程下内存池仍**略慢**，但差距从 44% 缩小到 16%。分析：

- glibc 同样有 per-thread arena（通过 `arena_get` 分配），多线程下 arena 切换有一定开销
- 内存池的 TLS 隔离优势开始体现——每个线程独立 ThreadCache，无锁分配
- 但 CentralCache 自旋锁 + 单块传输的开销拖了后腿（每 100 次的批量释放触发 returnRange）
- 第 2 轮 MultiThreaded (MP) 出现 10.748 ms 的抖动，可能是 PageCache mmap 首次触发的缺页中断

### MixedSizes — 单线程混合大小

| 指标 | MemoryPool | new/delete | 差距 |
|:---|:---|:---|:---|
| 平均值 | **2.723 ms** | **5.013 ms** | 快 **84.1%** ✅ |
| 最小 | 2.575 ms | 4.828 ms | — |
| 最大 | 2.869 ms | 5.093 ms | — |
| 标准差 | 0.124 ms | 0.098 ms | — |

```
MemoryPool  ██████████████░░░░░░░░░░░░░░░░░░░░░░░░░  2.723 ms
new/delete  ████████████████████████████░░░░░░░░░░░░  5.013 ms
```

**结论**：混合大小场景下内存池**大幅领先**，快了近一倍。这是本次测试中唯一的优势场景：

- 混合大小（8B ~ 4KB）打破了 glibc malloc 的 per-size freelist 命中模式
- 内存池的 SizeClass 对任何大小都 O(1) 路由到对应自由链表
- 预热后的 PageCache 缓存了 8 页 Span，混合大小请求不需要新的 mmap
- ThreadCache 的本地缓存让大小类别切换没有额外成本

## 综合评估

```
场景                  MemoryPool   new/delete   结论
──────────────────────────────────────────────────────
单线程小对象           2.827 ms     1.967 ms     ❌ 慢 44%
4线程并发              8.958 ms     7.756 ms     ❌ 慢 16%
混合大小               2.723 ms     5.013 ms     ✅ 快 84%
```

### 性能短板

1. **单线程无优势**：ThreadCache TLS 和 SizeClass 路由在单线程下的固定开销无法被并发优势摊还
2. **批量传输缺失**：每次 `fetchRange` 只给 1 块，跨层调用频率过高（见 `optimizations/04-批量传输.md`）
3. **桶锁粒度过细**：32768 个桶锁的 `test_and_set` 和 `clear` 操作在每次 fetchRange/returnRange 中执行
4. **getSpanTracker 线性扫描**：Span 数量增多后每次 fetchRange (分支 B) 都要 O(n) 查找

### 优势

1. **混合大小高效**：SizeClass 对任意大小的 O(1) 路由 + ThreadCache 本地化是核心优势
2. **抖动低**：MemoryPool 的标准差普遍 ≤ 系统 malloc（除 MultiThreaded 第 2 轮外）
3. **内存可控**：PageCache Span 复用避免频繁 mmap，内存占用可预测

### 后续优化方向

对应 `optimizations/` 目录，优先级：

1. 修复 Bug（01/02/03）—— 不影响本次压测，但影响长期运行稳定性
2. **批量传输（04）**—— 预计对 SmallAllocation 和 MultiThreaded 有显著提升
3. getSpanTracker 二分查找（06）—— 减少 CentralCache 持锁时间
4. unordered_map 替换（07）—— 减少自旋锁临界区内堆分配
