# returnToCentralCache 链表短于计数器时静默失败

## 严重程度

**Bug —— 计数器偏差时陷入死循环，每次 deallocate 空转**

## 位置

`src/ThreadCache.cpp` `returnToCentralCache`

## 问题描述

`returnToCentralCache` 在遍历链表找分割点时，如果链表实际长度 < `freeListSize_` 计数器值，`splitNode` 会变成 `nullptr`，导致整个归还逻辑被跳过，`freeListSize_` 得不到修正。

```cpp
// 原代码问题
for (size_t i = 0; i < keepNum - 1; ++i)
{
    splitNode = reinterpret_cast<char*>(*reinterpret_cast<void**>(splitNode));
    if (splitNode == nullptr)
    {
        returnNum = batchNum - (i + 1);  // 调整了归还数
        break;                            // 但 splitNode 已经是 nullptr！
    }
}

if (splitNode != nullptr)   // ← FALSE！归还逻辑全部跳过
{
    // 断开链表、更新 freeList_[index]、更新 freeListSize_[index]、
    // 调 CentralCache::returnRange
    // → 全部不执行！
}
```

## 复现路径

```
前提：freeListSize_[index] = 257，但链表实际只有 3 个节点
     （计数器因某种原因出现偏差）

Step 1: deallocate → freeListSize_[index]++ = 258
        shouldReturnToCentralCache → true (258 > 256)

Step 2: returnToCentralCache:
        batchNum=258, keepNum=64
        遍历链表找第 64 个节点
        i=0: 第2节点 ✓
        i=1: 第3节点 ✓
        i=2: 第4节点 = nullptr ← 断了！
        splitNode = nullptr, break

Step 3: if (splitNode != nullptr) → FALSE
        整个归还逻辑跳过，freeListSize_ 仍然是 258

Step 4: 下次 deallocate → freeListSize_ = 259 → 再次触发
        returnToCentralCache → 再次遍历 3 个节点 → 再次静默失败
        → 无限循环！
```

## 触发条件

正常情况下 `freeListSize_` 和链表长度一致（修复 #02 保证）。但一旦出现偏差——例如 `fetchFromCentralCache` 返回了意外长度的链表、内存被意外覆写——函数不仅无法自愈，还会陷入每次 `deallocate` 都空转的死循环。

## 修复（已应用）

### 核心思路

不再依赖 `splitNode != nullptr` 做分支判断。无论链表多短，都：
1. 用实际遍历到的节点数（`actualKeep`）替代预期值
2. 始终更新 `freeListSize_` 为实际值（偏差自愈）
3. 始终执行断开和归还逻辑

### 变更

**替换前：**
```cpp
char* current = static_cast<char*>(start);      // 无用变量
char* splitNode = current;
for (size_t i = 0; i < keepNum - 1; ++i)
{
    splitNode = reinterpret_cast<char*>(*reinterpret_cast<void**>(splitNode));
    if (splitNode == nullptr)
    {
        returnNum = batchNum - (i + 1);          // 调整归还数
        break;                                    // 但 keepNum 没调！
    }
}

if (splitNode != nullptr)                        // ← 链表短时跳过
{
    void* nextNode = *reinterpret_cast<void**>(splitNode);
    *reinterpret_cast<void**>(splitNode) = nullptr;
    freeList_[index] = start;
    freeListSize_[index] = keepNum;              // 用的是预期值！
    if (returnNum > 0 && nextNode != nullptr)
        CentralCache::getInstance().returnRange(...);
}
// splitNode==nullptr → 什么都不做 → freeListSize_ 未修正 → 死循环
```

**替换后：**
```cpp
char* splitNode = static_cast<char*>(start);
size_t actualKeep = 1;                           // 实际遍历到的节点数
for (size_t i = 0; i < keepNum - 1; ++i)
{
    void* next = *reinterpret_cast<void**>(splitNode);
    if (next == nullptr)
        break;                                   // 链表比计数器短，以实际为准
    splitNode = static_cast<char*>(next);
    actualKeep++;                                // 跟踪实际保留数
}

// 无论链表长短，都执行断开和状态更新
void* nextNode = *reinterpret_cast<void**>(splitNode);
*reinterpret_cast<void**>(splitNode) = nullptr;

size_t actualReturn = (batchNum > actualKeep) ? (batchNum - actualKeep) : 0;

freeList_[index] = start;
freeListSize_[index] = actualKeep;               // 用实际值修正计数器

if (actualReturn > 0 && nextNode != nullptr)
    CentralCache::getInstance().returnRange(nextNode, actualReturn * alignedSize, index);
```

### 边界验证

| 场景 | batchNum | 实际节点 | keepNum | actualKeep | 行为 |
|:---|:---|:---|:---|:---|:---|
| 正常 | 257 | 257 | 64 | 64 | 保留64，归还193 ✓ |
| 链表短 | 257 | 3 | 64 | 3 | 保留3，归还254，freeListSize_=3 ✓ |
| 刚好 | 2 | 2 | 1 | 1 | 保留1，归还1 ✓ |
| 单节点 | 2 | 1 | 1 | 1 | nextNode=null，不归但 freeListSize_=1 ✓ |

## 影响

计数器偏差场景下从"死循环空转"变为"一次性自愈"。正常路径行为不变，仅多了一个 `actualKeep` 变量的维护，开销可忽略。
