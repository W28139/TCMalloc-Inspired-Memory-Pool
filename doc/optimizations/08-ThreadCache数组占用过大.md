# ThreadCache 每个线程的数组占用过大（~512KB/线程）

## 严重程度

**内存优化 —— 线程数多时浪费严重**

## 位置

`include/ThreadCache.h` `freeList_` + `freeListSize_`

## 问题描述

```cpp
std::array<void*, FREE_LIST_SIZE>  freeList_;      // 32768 × 8B = 256KB
std::array<size_t, FREE_LIST_SIZE> freeListSize_;   // 32768 × 8B = 256KB
```

每个线程的 ThreadCache 实例占用约 **512KB** TLS 空间。如果程序有 100 个线程：

```
100 × 512KB = 51.2MB
```

而绝大多数线程实际只用其中几个大小类别（例如 8B、16B、32B、64B），剩余 32700+ 个槽位永远是空的。

## 对比

TCMalloc 实际实现中，ThreadCache 使用**稀疏数组**或**按需分配**策略。对于 256KB 上限用 32768 个槽位的设计本身就是一种"空间换时间"的极端做法——在小线程数场景 OK，但不适合大量线程。

## 修复方案

### 方案 A：缩小 FREE_LIST_SIZE 并改为两级映射

```cpp
// 常见小对象: 8B ~ 32KB → 精细粒度 (8B 步进)
// 大对象: 32KB ~ 256KB → 粗粒度 (4KB 步进)

constexpr size_t SMALL_MAX = 32 * 1024;      // 32KB
constexpr size_t SMALL_SIZE = SMALL_MAX / ALIGNMENT;  // 4096 个槽位

constexpr size_t LARGE_MAX = MAX_BYTES;       // 256KB
constexpr size_t LARGE_STEP = 4096;           // 4KB 步进
constexpr size_t LARGE_SIZE = (LARGE_MAX - SMALL_MAX) / LARGE_STEP; // 56 个槽位

// ≈ 4152 个槽位，而非 32768
```

### 方案 B：只分配使用到的槽位（按需扩容）

```cpp
class ThreadCache {
    // 使用 vector 而非 array，初始大小很小
    std::vector<void*> freeList_{64, nullptr};     // 初始 64 个槽
    std::vector<size_t> freeListSize_{64, 0};

    void ensureCapacity(size_t index) {
        if (index >= freeList_.size()) {
            freeList_.resize(index + 64, nullptr);
            freeListSize_.resize(index + 64, 0);
        }
    }
};
```

代价：增加一次 resize 分支判断。

### 方案 C：分级——频繁使用的小 size 用数组，大 size 用 map

```cpp
std::array<void*, 512> hotFreeList_;       // 8B ~ 4KB，覆盖大多数分配
std::map<size_t, void*> coldFreeList_;     // > 4KB，冷数据
```

## 收益

100 线程场景：51MB → ~3MB
