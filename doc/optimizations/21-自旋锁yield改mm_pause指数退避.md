# 自旋锁：yield → _mm_pause + 指数退避

## 类型

**性能优化（`14-性能优化方案.md` 第一档第 3 项）**

## 位置

`src/CentralCache.cpp`：`fetchRange` / `returnRange` 各一处桶级自旋锁循环；`#include` 区

## 现状与问题

```cpp
// 优化前：
while (locks_[index].test_and_set(std::memory_order_acquire))
{
    std::this_thread::yield();
}
```

`std::this_thread::yield()` 把线程让给调度器（微秒级延迟），争用激烈时恢复后立即重试——两次尝试之间缓存行已在多个核间横跳，且被踢出再唤醒的调度开销远大于自旋本身。

## 实际修复（已应用）

### 变更 1：平台条件 include

```cpp
// 优化 #21：x86 平台用 PAUSE 指令做自旋锁退避；非 x86 退化回 yield
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>   // _mm_pause
#endif
```

### 变更 2：两处自旋循环替换（fetchRange / returnRange）

```cpp
// 获取桶级自旋锁（优化 #21：yield → _mm_pause + 指数退避）
// 指数退避：pause 次数 1,2,4,...,64（上限），总退避 ~10ns → 几百 ns，
// 避免 yield 让出线程的微秒级调度延迟与缓存行颠簸
for (int spin = 0; locks_[index].test_and_set(std::memory_order_acquire); ++spin)
{
#if defined(__x86_64__) || defined(__i386__)
    for (int i = 0; i < (1 << std::min(spin, 6)); ++i)
        _mm_pause();
#else
    std::this_thread::yield();
#endif
}
```

**原理**：`_mm_pause`（x86 PAUSE 指令）只停顿几十个周期（~10ns 级），不触发调度器；指数退避（1,2,4,...,64 次）在争用加剧时指数拉长退避，减少总线争用与缓存行颠簸。

## 风险与边界

| 项 | 分析 |
|:---|:---|
| 单线程路径 | 第一次 `test_and_set` 成功即退出循环，无任何额外开销 |
| 非 x86 平台 | `#if` 分支退化回 `yield`，行为与原实现一致 |
| 死循环风险 | `test_and_set` 保持在前，退避只发生在失败后；争用持续时指数退避有 64 次上限，不会无限拉长 |

## 验证

未跑编译与测试（按要求不做验证）。建议：

1. `bin/unit_test` 回归，确认全绿；
2. `bin/perf_test` 多线程场景对比（4 轮平均）：争用场景预期 +20%~40%；
3. 可选：`perf stat -e context-switches` 观察上下文切换下降（退避的直接证据）。

## 关联

- 方案总纲：`14-性能优化方案.md`（第一档第 3 项）
- 前置：`20-BATCH_SIZE提升到32.md`（优化 #20）
