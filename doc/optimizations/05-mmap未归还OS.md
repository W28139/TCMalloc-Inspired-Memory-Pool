# 内存永不归还操作系统

## 严重程度

**设计缺陷 —— 长期运行进程内存只增不减**

## 位置

`src/PageCache.cpp` `deallocateSpan`

## 问题描述

PageCache 回收 Span 后，将其插入 `freeSpans_` 缓存，等待下次分配复用。但**没有任何地方调用 `munmap`** 把内存真正还给 OS。

```
进程生命周期:
  启动 → mmap 分配 → 用完后缓存 → mmap 更多 → 缓存 → ...
  内存 RSS: 只增不减
```

对于短期程序无所谓（进程退出时 OS 回收），但长期运行的服务进程（如数据库、Web 服务器）会持续堆高内存占用，即使业务空闲也不释放。

## 修复方案

在 `deallocateSpan` 末尾增加内存水位线检查：

```cpp
void PageCache::deallocateSpan(void* ptr, size_t numPages)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // ... 现有合并逻辑 ...

    // 插入 freeSpans_
    auto& list = freeSpans_[span->numPages];
    span->next = list;
    list = span;

    // ===== 新增：内存水位线检查 =====
    cachedPages_ += span->numPages;

    // 如果缓存超过阈值（如 128MB = 32768 页），释放部分
    if (cachedPages_ > MAX_CACHED_PAGES)
    {
        releaseExcessSpans();
    }
}

void PageCache::releaseExcessSpans()
{
    // 从最大的 Span 开始释放（减少碎片）
    for (auto it = freeSpans_.rbegin();
         it != freeSpans_.rend() && cachedPages_ > MAX_CACHED_PAGES / 2;
         /* ... */)
    {
        Span* span = it->second;
        while (span && cachedPages_ > MAX_CACHED_PAGES / 2)
        {
            Span* next = span->next;
            munmap(span->pageAddr, span->numPages * PAGE_SIZE);
            spanMap_.erase(span->pageAddr);
            cachedPages_ -= span->numPages;
            delete span;
            span = next;
        }
        // ...
    }
}
```

## 配置项

- `MAX_CACHED_PAGES`：建议默认 32768（128MB），可通过环境变量或 API 调整
