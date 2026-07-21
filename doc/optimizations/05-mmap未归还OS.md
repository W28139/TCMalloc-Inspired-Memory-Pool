# 内存永不归还操作系统

## 严重程度

**设计缺陷 —— 长期运行进程内存只增不减**

## 位置

`src/PageCache.cpp` `deallocateSpan` + `allocateSpan`

## 问题描述

PageCache 回收 Span 后，将其插入 `freeSpans_` 缓存等待复用。但**没有任何地方调用 `munmap`** 把内存还给 OS。

```
进程生命周期:
  启动 → mmap 分配 → 用完后缓存 → mmap 更多 → 缓存 → ...
  内存 RSS: 只增不减，即使业务完全空闲
```

对于短期程序无所谓（进程退出时 OS 回收），但长期运行的服务进程会持续堆高内存占用。

## 影响范围

- 每个 Span 默认 8 页（32KB），1000 个闲置 Span = 32MB 无法释放
- 长期运行 + 曾经有过内存高峰 → RSS 永远停在峰值
- Web 服务器、数据库等场景下不可接受

---

## 实际修复（已应用）

### 设计思路

添加内存水位线机制：`cachedPages_` 追踪 `freeSpans_` 中缓存的页数，超过阈值 `MAX_CACHED_PAGES`（128MB）时，从最大的 Span 开始 `munmap` 释放，降到阈值的一半（64MB）为止。

- 大 Span 优先释放：减少碎片，保留小 Span 供后续小请求命中
- 降到一半而非阈值线：留出缓冲，避免频繁触发释放→重新申请

### 变更 1：头文件 `include/PageCache.h`

```cpp
// 新增
void releaseExcessSpans();                              // 超阈值释放
static const size_t MAX_CACHED_PAGES = 32768;           // 128MB 阈值
size_t cachedPages_{0};                                 // 当前缓存页数
```

### 变更 2：`allocateSpan` — 取走 Span 时扣减计数

**分支 A（从 freeSpans_ 取）：**
```cpp
// 取走整个 Span，从缓存计数中扣除
cachedPages_ -= span->numPages;

// ... 从 freeSpans_ 摘除 ...

// 如果切分，剩余部分放回缓存
if (span->numPages > numPages) {
    // ... 创建 newSpan ...
    cachedPages_ += newSpan->numPages;  // 剩余部分回到缓存
}
```

**分支 B（systemAlloc）：** 无需改动，新内存不经过缓存，不计入 `cachedPages_`。

### 变更 3：`deallocateSpan` — 归还后增加计数并检查水位线

```cpp
// 插入 freeSpans_（原有逻辑）
auto& list = freeSpans_[span->numPages];
span->next = list;
list = span;

// 新增：更新缓存计数
cachedPages_ += span->numPages;

// 新增：超阈值时释放
if (cachedPages_ > MAX_CACHED_PAGES)
{
    releaseExcessSpans();
}
```

### 变更 4：新增 `releaseExcessSpans()` 方法

```cpp
void PageCache::releaseExcessSpans()
{
    size_t targetPages = MAX_CACHED_PAGES / 2;  // 降到 64MB

    // 从最大的 Span 开始（reverse_iterator），优先释放大块
    for (auto it = freeSpans_.rbegin();
         it != freeSpans_.rend() && cachedPages_ > targetPages; )
    {
        Span* span = it->second;
        while (span && cachedPages_ > targetPages)
        {
            Span* next = span->next;

            munmap(span->pageAddr, span->numPages * PAGE_SIZE);  // 归还 OS
            spanMap_.erase(span->pageAddr);                       // 移除映射
            cachedPages_ -= span->numPages;                       // 扣减计数
            delete span;                                          // 释放控制块

            span = next;
        }
        it->second = span;

        if (span == nullptr)
        {
            // 链表已空，从 map 删除该页数条目
            it = std::map<size_t, Span*>::reverse_iterator(
                freeSpans_.erase(std::next(it).base()));
        }
        else
        {
            ++it;
        }
    }
}
```

> **为什么从大 Span 开始释放？** 大 Span（如 8 页、16 页）不容易命中精确匹配的请求，缓存价值低。小 Span（1~2 页）灵活性高，优先保留。同时大 Span 释放能更快降低 `cachedPages_`。

---

## 压测对比

| 场景 | Fix#4 后 | Fix#5 后 | 变化 |
|:---|:---|:---|:---|
| SmallAllocation | 2.571 ms | 2.657 ms | +3.4% |
| MultiThreaded | 6.443 ms | 6.392 ms | −0.8% |
| MixedSizes | 2.555 ms | 2.479 ms | −3.0% |

> Fix#5 后 10 轮测试，`bin/perf_test`（WSL2, g++ -O2, C++17）

**结论：性能无影响。** 压测内存使用量远低于 128MB 阈值，`releaseExcessSpans` 从未被触发，`cachedPages_` 的加减只是两次整数运算。水位线机制不影响正常路径的性能，仅在长期运行进程的内存缓存超过 128MB 时才会介入释放。
