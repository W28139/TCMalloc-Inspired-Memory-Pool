# PageCache 切分后的剩余 Span 未登记 spanMap_，后向合并失效

## 严重程度

**严重问题（P1-3，来自 `13-review.md`）—— 切分场景下外部碎片无法完全对抗，长期运行内存膨胀**

## 位置

`src/PageCache.cpp` `allocateSpan` 分支 A（切分逻辑，修复前 :66-84）

## 问题描述

Best-Fit 找到的空闲 Span 大于请求时切分：前 `numPages` 页返回，剩余部分（`newSpan`）放回 `freeSpans_` 缓存，**但没有登记 `spanMap_`**：

```cpp
// 修复前（有缺陷）：
if (span->numPages > numPages)
{
    Span* newSpan = new Span;
    newSpan->pageAddr = static_cast<char*>(span->pageAddr) + numPages * PAGE_SIZE;
    newSpan->numPages = span->numPages - numPages;
    newSpan->next = nullptr;

    auto& list = freeSpans_[newSpan->numPages];   // ← 只进了 freeSpans_
    newSpan->next = list;
    list = newSpan;
    cachedPages_ += newSpan->numPages;

    span->numPages = numPages;
}
spanMap_[span->pageAddr] = span;                  // ← 只登记了返回的前半部分
```

`deallocateSpan` 的后向合并依赖 `spanMap_.find(nextAddr)` 定位相邻 Span（`PageCache.cpp:135`）：

- 切分出的剩余部分**从未被分配过**就遇到相邻 Span 归还时，`spanMap_` 中查不到它 → 无法合并；
- 只有等它自己被分配、归还过一次（此时才进 `spanMap_`）后，合并才恢复工作。

**后果**：切分场景下外部碎片无法完全对抗，长期运行内存膨胀（碎片累积，非泄漏、非崩溃）。

## 实际修复（已应用）

### 变更：切分时登记 `spanMap_`

**替换后（已应用）：**
```cpp
            // 修复 P1-3：剩余 Span 必须登记 spanMap_。
            // 否则 deallocateSpan 的后向合并靠 spanMap_.find(nextAddr) 定位相邻 Span，
            // 切分出的剩余部分从未被分配过时找不到 → 无法合并 → 外部碎片无法对抗。
            // 合并逻辑本身无需改动：摘除检查（nextList == nextSpan）区分空闲/在用，
            // 被合并时 spanMap_.erase(nextAddr) 已清理条目。
            spanMap_[newSpan->pageAddr] = newSpan;
```

### 合并逻辑无需改动的依据

- `deallocateSpan` 摘除检查：`nextList == nextSpan`（空闲链表头）或链表中搜索——若 `nextSpan` 在用（不在 `freeSpans_`），`found = false`，不合并，行为正确；
- 被合并时：`spanMap_.erase(nextAddr); delete nextSpan;` 已清理 map 条目与控制块；
- `newSpan` 后续被分配时 `spanMap_[newSpan->pageAddr] = newSpan` 覆盖同一值，无冲突。

## 验证

未跑编译与测试（按要求不做验证）。建议：

1. `bin/unit_test` 回归，确认全绿；
2. 切分-归还循环压测：混合大小分配/释放（触发 Best-Fit 切分），观察缓存水位稳定（不碎片化增长）。

## 关联

- 完整 review：`13-review.md`（P1-3；相邻问题 P1-4 `deallocateSpan` 的 `operator[]` 空条目未修）
- 前置修复：`15-expandTrackerArray扩容排序覆盖活跃SpanTracker.md`（P0-1）、`16-Span归还后SpanTracker未失效.md`（P0-2）、`17-并发下trackerArray与sortedCount组合读取窗口.md`（P1-2）
