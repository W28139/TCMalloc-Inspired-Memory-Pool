# freeListSize_ 无符号整数下溢

## 严重程度

**Bug —— 计数器不准确，回收逻辑偶尔失效**

## 位置

`src/ThreadCache.cpp` `allocate` 第 25 行

## 问题描述

```cpp
void* ThreadCache::allocate(size_t size)
{
    size_t index = SizeClass::getIndex(size);

    freeListSize_[index]--;          // ← 这里！无论 freeList_ 有没有货都减

    if (void* ptr = freeList_[index])
    {
        // 快速路径：从 freeList_ 取走一块
        freeList_[index] = *reinterpret_cast<void**>(ptr);
        return ptr;
    }

    return fetchFromCentralCache(index);  // 慢速路径
}
```

`freeListSize_` 的类型是 `size_t`（无符号整数）。当它为 0 且 `freeList_[index]` 为空时：

```
freeListSize_[index]-- → 0 - 1 → SIZE_MAX (18446744073709551615)
```

随后 `fetchFromCentralCache` 中：
```cpp
freeListSize_[index] += batchNum;  // batchNum = 1
// SIZE_MAX + 1 → 0（回绕）
```

计数器短暂变为 `SIZE_MAX` 后又恢复为 0。虽然最终值是对的，但有两个隐患：

1. **多线程可见性问题**：如果有外部代码读取 `freeListSize_`（用于统计或调试），可能看到 `SIZE_MAX`
2. **逻辑脆弱**：如果 `fetchFromCentralCache` 失败返回 `nullptr`，计数器就卡在 `SIZE_MAX`，后续所有 `shouldReturnToCentralCache` 的判断都会出错

## 修复方案

只在实际取到块时才递减：

```cpp
void* ThreadCache::allocate(size_t size)
{
    if (size == 0) size = ALIGNMENT;
    if (size > MAX_BYTES) return malloc(size);

    size_t index = SizeClass::getIndex(size);

    // 先检查 freeList_ 是否有货
    if (void* ptr = freeList_[index])
    {
        freeList_[index] = *reinterpret_cast<void**>(ptr);
        freeListSize_[index]--;     // ← 移到成功取块之后
        return ptr;
    }

    // freeList_ 为空，计数器不动
    return fetchFromCentralCache(index);
}
```
