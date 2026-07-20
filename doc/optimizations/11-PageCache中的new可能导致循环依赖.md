# PageCache 中的 `new` 可能导致循环依赖

## 问题概述

`PageCache.cpp` 中有两处使用 `new Span` 在堆上分配 Span 控制块。虽然 `systemAlloc` 用 `mmap` 绕过了 `malloc`，但 `new` 本质上就是 `malloc` 的封装。如果用户将此内存池作为 `malloc` 的替代品（如通过 `LD_PRELOAD`），会形成循环依赖 → 死锁。

## 涉及代码

```cpp
// PageCache.cpp:156 —— allocateSpan 中的 Span 切分
Span* newSpan = new Span;

// PageCache.cpp:209 —— allocateSpan 中从 OS 申请后创建控制块
Span* span = new Span;
```

此外，以下隐式堆分配同样受影响：

| 位置 | 代码 | 堆分配来源 |
|:---|:---|:---|
| `ThreadCache.cpp:18` | `malloc(size)` | 大对象直接走系统 malloc |
| `PageCache.cpp` | `spanMap_[memory] = span` | `std::map::operator[]` 内部 `new` 树节点 |
| `PageCache.cpp` | `freeSpans_[n]` | `std::map::operator[]` 内部 `new` 树节点 |
| `CentralCache.cpp:209` | `std::unordered_map<...>` | `unordered_map` 内部 `new` 桶节点 |

## 死锁路径

当前 `malloc` 没有被重载，所以 `new` 走的是系统原生分配器，不存在循环。但一旦将内存池作为 `malloc` 替代品：

```
PageCache::allocateSpan(numPages)
  │  mutex_.lock()                        ← 第 1 次获取
  │
  ├── Span* span = new Span;              ← 24 字节，进入内存池
  │     │
  │     └── MemoryPool::allocate(24)
  │           │
  │           └── ThreadCache::allocate(24)
  │                 │
  │                 ├── freeList_[2] 有货？
  │                 │   └── YES → 直接返回（运气好，不死锁）
  │                 │
  │                 └── NO → fetchFromCentralCache(2)
  │                           │
  │                           └── CentralCache::fetchRange(2)
  │                                 │
  │                                 ├── centralFreeList_[2] 有货？
  │                                 │   └── YES → 返回（运气好，不死锁）
  │                                 │
  │                                 └── NO → fetchFromPageCache(24)
  │                                             │
  │                                             └── PageCache::allocateSpan
  │                                                   │
  │                                                   └── mutex_.lock()  ← 同一线程再次获取
  │                                                       非递归锁 → 死锁！💀
  │
  ├── spanMap_[memory] = span;           ← std::map 插入 → new 树节点
  │     └── 同上，可能进入内存池 → 死锁
  │
  └── freeSpans_[n] = ...;               ← std::map operator[] → new 树节点
        └── 同上
```

## 严重程度评估

### 当前状态（malloc 未被重载）

| 维度 | 评估 |
|:---|:---|
| 运行时影响 | **无**。`new` 走系统 malloc，不会回调内存池 |
| 设计一致性 | **有缺陷**。`systemAlloc` 用 mmap 避开 malloc，但 `new Span` 又把这条路打开了 |
| 分类 | 设计隐患 / 潜在 bug |

### 如果作为 malloc 替代品

| 维度 | 评估 |
|:---|:---|
| 触发条件 | Span 控制块分配时，ThreadCache 和 CentralCache 恰好都没有 24 字节的缓存 |
| 触发概率 | 低（Span 控制块 24 字节很小，通常在 ThreadCache 命中），但非零 |
| 后果 | 死锁，整个进程挂起 |
| 分类 | **严重 bug** |

## 修复方案

### 方案 A：用 mmap 分配 Span 控制块（推荐）

```cpp
// 替代 new Span
Span* PageCache::createSpan()
{
    // Span 结构体只有 24 字节，但 mmap 最小粒度是 1 页（4096B）
    // 可以设计一个简单的 Span 控制块池
    void* mem = mmap(nullptr, PAGE_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return nullptr;
    return new(mem) Span;  // Placement new，不分配内存
}

// 替代 delete span
void PageCache::destroySpan(Span* span)
{
    span->~Span();
    munmap(span, PAGE_SIZE);
}
```

缺点：每个 Span 控制块浪费几乎一整页（24B 用 4096B）。

### 方案 B：预分配 Span 控制块数组

```cpp
class PageCache
{
private:
    static const size_t MAX_SPANS = 1024;
    Span spanPool_[MAX_SPANS];     // 栈/静态区，不经过堆
    size_t spanPoolUsed_ = 0;

    Span* allocSpan()
    {
        if (spanPoolUsed_ < MAX_SPANS)
            return &spanPool_[spanPoolUsed_++];
        return nullptr;  // 超出容量，需要 fallback
    }
};
```

优点：零堆分配，彻底避开循环依赖。
缺点：Span 数量有硬上限（不过 1024 通常够用）。

### 方案 C：使用递归互斥锁（不推荐，治标不治本）

```cpp
std::recursive_mutex mutex_;  // 允许同一线程重复加锁
```

只解决死锁，不解决设计上的循环依赖——内存池底层不应该依赖内存池自己。

## 相关文件

| 文件 | 需修改的位置 |
|:---|:---|
| `src/PageCache.cpp` | 2 处 `new Span` + 1 处 `delete nextSpan` |
| `src/PageCache.cpp` | `spanMap_` / `freeSpans_` 的 `std::map` 内部 `new` |
| `src/ThreadCache.cpp` | `malloc(size)` 大对象分支 |

## 备注

真正的 TCMalloc 同样面临这个问题。它的解决方案是：
1. Span 控制块从**自己管理的特殊页**（metadata pages）中分配，不经过 malloc
2. 使用基数树（Radix Tree）而非 `std::map`，基数树的节点也预分配在元数据区
3. 启动时就向 OS 申请足够的元数据内存，运行时不再产生新的堆分配

这个项目用 `new` 和 `std::map` 作为教学简化的折中——在没有重载 malloc 的前提下是安全的，但要演进为真正的 malloc 替代品就必须解决。
