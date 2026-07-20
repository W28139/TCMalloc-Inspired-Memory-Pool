# 测试说明

两个可执行文件，编译后输出到 `bin/`：

| 可执行文件 | 源文件 | 编译命令 | 运行命令 |
|:---|:---|:---|:---|
| `unit_test` | `UnitTest.cpp` | `make unit_test` | `make test` 或 `./bin/unit_test` |
| `perf_test` | `PerformanceTest.cpp` | `make perf_test` | `make perf` 或 `./bin/perf_test` |

---

## 一、unit_test —— 单元测试（正确性验证）

**目标**：验证内存池分配/释放的**功能是否正确**，不关心性能。

### 测试用例

#### 1. testBasicAllocation —— 基础分配

| 参数 | 值 |
|:---|:---|
| 测试次数 | 3 次（小/中/大各一次） |

| 序号 | 请求大小 | 命中层级 | 验证点 |
|:---|:---|:---|:---|
| 1 | 8B | ThreadCache → CentralCache → PageCache（小对象） | 非空 |
| 2 | 1024B (1KB) | ThreadCache → CentralCache（中等对象） | 非空 |
| 3 | 1MB | **直接 malloc**（超过 MAX_BYTES=256KB） | 非空 |

**验证方式**：`assert(ptr != nullptr)` — 只检查分配成功，不检查数据完整性。

---

#### 2. testMemoryWriting —— 数据完整性

| 参数 | 值 |
|:---|:---|
| 测试次数 | 1 次 |
| 请求大小 | 128B |
| 写入内容 | `ptr[i] = i % 256`（0x00 ~ 0xFF 循环） |
| 释放方式 | 先验证再释放 |

**验证方式**：写入后立即回读，逐字节断言 `ptr[i] == i % 256`。确认：
- 分配的内存确实可读写
- 写入的数据未被破坏（排除悬垂指针、越界等问题）

---

#### 3. testMultiThreading —— 多线程并发

| 参数 | 值 |
|:---|:---|
| 线程数 | **4** |
| 每线程分配次数 | **1000** |
| 总分配次数 | 4000 |
| 大小范围 | `(rand() % 256 + 1) × 8` → **8B ~ 2048B** |
| 释放节奏 | 每次分配后有 **50% 概率**随机释放一个已分配的块 |
| 错误标记 | `std::atomic<bool> has_error` |

**验证方式**：
- 每个线程独立记录自己分配的所有块（地址 + 大小）
- 分配失败 → 设置 `has_error` → 同线程停止分配
- 线程退出前释放剩余块
- 所有线程 join 后检查无异常

**压力点**：4 个线程同时竞争相同大小类别的 CentralCache 自旋锁。

---

#### 4. testEdgeCases —— 边界条件

| 序号 | 请求大小 | 预期行为 | 验证点 |
|:---|:---|:---|:---|
| 1 | **0B** | 内部修正为 8B，返回非空 | `assert(ptr != nullptr)` |
| 2 | **1B** | 分配 8B（向上取整），地址 **8 字节对齐** | `assert((addr & 7) == 0)` |
| 3 | **MAX_BYTES (256KB)** | 内存池管理范围的最大值，成功分配 | 非空 |
| 4 | **MAX_BYTES+1 (256KB+1B)** | 超出范围 → 走 `malloc`，成功分配 | 非空 |

**验证方式**：验证内存池对极端输入的处理是否正确——不会崩溃、不会返回 null、对齐正确。

---

#### 5. testStress —— 压力测试

| 参数 | 值 |
|:---|:---|
| 分配次数 | **10000** |
| 大小范围 | `(rand() % 1024 + 1) × 8` → **8B ~ 8KB** |
| 释放方式 | 全部分配完后，**随机打乱顺序**一次性释放 |

**验证方式**：
- 10000 次分配全部成功
- 随机乱序释放不崩溃

**压力点**：随机乱序释放导致 CentralCache 的 `centralFreeList_` 链表高度碎片化，不同 Span 的空闲块交错排列，考验 `performDelayedReturn` 和 `updateSpanFreeCount` 的处理能力。

---

### 测试矩阵总览

| 测试 | 大小范围 | 数量 | 线程 | 释放方式 | 主要验证层 |
|:---|:---|:---|:---|:---|:---|
| BasicAllocation | 8B / 1KB / 1MB | 3 | 1 | 立即释放 | 三层 + malloc 分支 |
| MemoryWriting | 128B | 1 | 1 | 验证后释放 | 数据完整性 |
| MultiThreading | 8B~2KB | 4000 | 4 | 50% 概率随机 | ThreadCache TLS + CentralCache 锁 |
| EdgeCases | 0 / 1B / 256KB / 256KB+1 | 4 | 1 | 立即释放 | 边界处理 + 对齐 |
| Stress | 8B~8KB | 10000 | 1 | 全部乱序释放 | 碎片化回收 |

---

## 二、perf_test —— 性能测试（基准对比）

**目标**：与系统 `new[]/delete[]` 对比，量化内存池在**不同场景**下的性能差异。

**对比基准**：`new char[size]` / `delete[]` — 系统默认分配器（glibc malloc）。

**计时单位**：毫秒（ms），使用 `std::chrono::high_resolution_clock`。

### 测试用例

#### 0. warmup —— 预热

| 参数 | 值 |
|:---|:---|
| 预热轮次 | 1000 |
| 每轮大小 | {8, 16, 32, 64, 128, 256, 512, 1024} × 8 种 |
| 总分配次数 | 1000 × 8 = **8000** |

**目的**：消除冷启动偏差——首次访问内存池时 ThreadCache TLS 尚未初始化、PageCache 无缓存 Span，这些一次性开销不应计入后续对比。

---

#### 1. testSmallAllocation —— 小对象高频

| 参数 | 值 |
|:---|:---|
| 总分配次数 | **50000** |
| 大小集合 | {8, 16, 32, 64, 128, 256} × 6 种，**循环使用** |
| 释放节奏 | 每 **4 次**分配释放 **1 次**（25%），随机选一个大小类别 |
| 对内存池的影响 | 主要在 **ThreadCache 命中**（小对象 + 固定大小 + TLS 隔离） |

**对比方式**：同样的分配/释放模式，分别计时：

```
Memory Pool:  xxx ms
New/Delete:   xxx ms
```

**预期**：内存池在小对象高并发场景下明显优于 `new/delete`。

---

#### 2. testMultiThreaded —— 多线程并发性能

| 参数 | 值 |
|:---|:---|
| 线程数 | **4** |
| 每线程分配次数 | **25000** |
| 总分配次数 | **100000** |
| 大小集合 | {8, 16, 32, 64, 128, 256} × 6 种，循环使用 |

**三个阶段的释放模式**（模拟真实应用）：

| 阶段 | 触发频率 | 操作 | 测试目的 |
|:---|:---|:---|:---|
| 常规释放 | 每 **100 次** | 随机选一个大小类别，释放其中 **20%~30%** | 测试 ThreadCache 回收 → CentralCache 延迟归还 |
| 内存压力 | 每 **1000 次** | 快速分配 50 块 → 立即全部释放 | 触发 CentralCache 竞争（4 线程同时） |

**对比方式**：

```
线程函数 threadFunc(bool useMemPool):
  useMemPool=true  → MemoryPool::allocate / deallocate
  useMemPool=false → new char[] / delete[]

分别计时两组 4 线程并发执行。
```

**压力点**：
- 4 线程同时竞争相同大小类别的 CentralCache 自旋锁
- ThreadCache 阈值回收（freeListSize_ > 256 → returnToCentralCache）
- CentralCache 延迟归还 → PageCache Span 合并

---

#### 3. testMixedSizes —— 混合大小

| 参数 | 值 |
|:---|:---|
| 总分配次数 | **100000** |
| 大小分布 | 小 60% / 中 30% / 大 10%（按 `i % 100` 确定） |

| 类别 | 占比 | 大小集合 | 命中层级 |
|:---|:---|:---|:---|
| **小对象** | 60% | {8, 16, 32, 64, 128} | ThreadCache（最快） |
| **中等对象** | 30% | {256, 384, 512} | CentralCache（桶锁） |
| **大对象** | 10% | {1024, 2048, 4096} | PageCache 或接近 Span 级别 |

**释放节奏**：每 **50 次**分配触发一次释放，随机选一个大小类别，释放其中 **20%~30%**。

**目的**：模拟真实应用的混合负载——少量大对象夹杂在海量小对象中。验证内存池在不同大小类别间切换时没有性能退化。

---

### 性能测试矩阵

| 测试 | 总次数 | 线程 | 大小范围 | 释放频率 | 核心对比维度 |
|:---|:---|:---|:---|:---|:---|
| SmallAllocation | 50000 | 1 | 8B~256B | 每 4 次放 1 次 | ThreadCache 命中率 |
| MultiThreaded | 100000 | 4 | 8B~256B | 每 100 次 20~30% + 压力脉冲 | 多线程锁竞争 |
| MixedSizes | 100000 | 1 | 8B~4KB (3 类) | 每 50 次 20~30% | 跨大小类切换 + PageCache |

---

## 三、运行环境要求

| 项目 | 要求 |
|:---|:---|
| 操作系统 | Linux (WSL / 原生) |
| C++ 标准 | C++17 |
| 编译器 | g++ (支持 `-std=c++17`) |
| 依赖库 | pthread |
| 构建系统 | CMake ≥ 3.10 |

## 四、如何添加新测试

添加新测试后，在 CMakeLists.txt 中注册：

```cmake
add_executable(new_test
    ${SOURCES}
    ${TEST_DIR}/NewTest.cpp
)
target_link_libraries(new_test PRIVATE Threads::Threads)
```
