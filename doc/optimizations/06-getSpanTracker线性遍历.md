# getSpanTracker 线性遍历 O(n)

## 严重程度

**性能优化 —— 每次 fetchRange (分支B) 都触发**

## 位置

`src/CentralCache.cpp` `getSpanTracker` 第 637-658 行

## 问题描述

```cpp
SpanTracker* CentralCache::getSpanTracker(void* blockAddr)
{
    for (size_t i = 0; i < spanCount_.load(...); ++i)  // 线性扫描
    {
        if (blockAddr >= spanAddr &&
            blockAddr < spanAddr + numPages * PAGE_SIZE)
        {
            return &spanTrackers_[i];
        }
    }
    return nullptr;
}
```

`fetchRange` 分支 B（从 cache 取块）**每次**都调用 `getSpanTracker` 来更新 `freeCount`。当活跃 Span 数量接近 1024 时，每次取块都要扫描数百个 SpanTracker。

## 复杂度分析

| Span 数量 | 每次 fetchRange (B) 的扫描次数 | 影响 |
|:---|:---|:---|
| 10 | 平均 5 次 | 可忽略 |
| 512 | 平均 256 次 | 明显 |
| 1024 | 平均 512 次 | **每次取块都多扫 500+ 个元素** |

## 修复方案

### 方案 A：按地址排序 + 二分查找（简单）

```cpp
// 插入 spanTrackers_ 时保持按 spanAddr 有序
// 查找时用二分
SpanTracker* CentralCache::getSpanTracker(void* blockAddr)
{
    size_t left = 0, right = spanCount_.load(...);
    while (left < right)
    {
        size_t mid = (left + right) / 2;
        void* addr = spanTrackers_[mid].spanAddr.load(...);
        size_t pages = spanTrackers_[mid].numPages.load(...);

        if (blockAddr < addr)
            right = mid;
        else if (blockAddr >= static_cast<char*>(addr) + pages * PageCache::PAGE_SIZE)
            left = mid + 1;
        else
            return &spanTrackers_[mid];
    }
    return nullptr;
}
```

代价：插入时需保持有序（插入排序或批量排序），但插入频率远低于查找。

### 方案 B：在内存块头部嵌入 SpanTracker 索引

在每个内存块的前 8 字节存储的不是 next 指针（空闲时）就是 SpanTracker 索引（分配后）。问题是：用户会覆盖这块内存。

### 方案 C：利用地址高位做哈希

如果从 PageCache 分配的 Span 地址有一定规律，可以用页号÷8 直接算出桶号。

## 收益预估

O(n) → O(log n)，Span 多时提速明显。
