# deallocateSpan 用 operator[] 访问 freeSpans_，对"在用"的相邻 Span 产生空条目

## 严重程度

**严重问题（P1-4，来自 `13-review.md`）—— map 无谓膨胀 + 潜在空指针解引用崩溃**

## 位置

`src/PageCache.cpp` `deallocateSpan`（后向合并的摘除检查，修复前 :144）

## 问题描述

`freeSpans_` 是 `std::map<size_t, Span*>`（页数 → 空闲链表头）。合并检查时用 `operator[]` 取"相邻 Span 页数桶"的链表头：

```cpp
// 修复前（有缺陷）：
auto nextIt = spanMap_.find(nextAddr);
if (nextIt != spanMap_.end())
{
    Span* nextSpan = nextIt->second;
    bool found = false;
    auto& nextList = freeSpans_[nextSpan->numPages];  // ← operator[]！
    ...
}
```

`std::map::operator[]` 的语义：**key 不存在时插入 `{key, nullptr}`**。当 `nextSpan` **正在使用中**（在 `spanMap_` 中但不在 `freeSpans_` 链表中）时，该页数桶可能从未存在 → 插入 `{numPages → nullptr}` 空条目。

**后果**：

1. **map 无谓膨胀**：每个"在用 Span 的页数"首次作为相邻项被检查时插一个空条目，长期运行 map 变大；`releaseExcessSpans` 的空条目清理只在缓存超 128MB 水位线时触发，空条目在此之前一直存活；
2. **潜在崩溃**：`allocateSpan` 的 `lower_bound` 会命中空条目：

```cpp
auto it = freeSpans_.lower_bound(numPages);
if (it != freeSpans_.end())
{
    Span* span = it->second;             // ← 空条目时 span == nullptr
    cachedPages_ -= span->numPages;      // ← 解引用空指针 → 崩溃！
}
```

任何"请求页数 ≤ 空条目页数 且没有更小空闲桶"的分配都会撞上。

## 实际修复（已应用）

### 变更：`operator[]` → `find`

**替换后（已应用）：**
```cpp
bool found = false;
auto listIt = freeSpans_.find(nextSpan->numPages);
if (listIt != freeSpans_.end())
{
    Span*& nextList = listIt->second;

    if (nextList == nextSpan)
    {
        nextList = nextSpan->next;
        found = true;
    }
    else if (nextList)
    {
        Span* prev = nextList;
        while (prev->next)
        {
            if (prev->next == nextSpan)
            {
                prev->next = nextSpan->next;
                found = true;
                break;
            }
            prev = prev->next;
        }
    }
}
```

`find` 找不到时不插入任何条目；`nextSpan` 在用（桶不存在）时直接跳过合并检查——这正是预期行为（在用 Span 本就不该被合并）。`nextList` 改为 `Span*&` 引用以保留头节点摘除的写回语义。

## 验证

未跑编译与测试（按要求不做验证）。建议：

1. `bin/unit_test` 回归，确认全绿；
2. 混合大小分配/归还循环，白盒断言：`freeSpans_` 中不应存在 value 为 nullptr 的条目。

## 关联

- 完整 review：`13-review.md`（P1-4）
- 同文件前置修复：`18-PageCache切分剩余Span未登记spanMap.md`（P1-3，同一段合并逻辑）
