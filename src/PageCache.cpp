/**
 * @file    PageCache.cpp
 * @brief   页缓存层 —— 直接与操作系统交互的底层内存管理者
 *
 * ── PageCache 在三层架构中的位置 ──
 *
 *   [ThreadCache]    ← 线程私有 TLS，无锁，分配第一站
 *        ↕
 *   [CentralCache]   ← 全局共享，桶级自旋锁，块级调度
 *        ↕
 *   [PageCache]      ← 全局共享，互斥锁，页级管理 ← 本文件
 *        ↕
 *   [OS Kernel]      ← mmap / munmap 系统调用
 *
 * ── 核心职责 ──
 *
 *   1. 以"页"（PAGE_SIZE = 4096B）为单位管理连续内存
 *   2. Span 切分：大 Span 切出所需部分，剩余缓存复用（Best-Fit）
 *   3. Span 合并：归还时检查后向相邻 Span，空闲则合并，对抗外部碎片
 *   4. 缓存管理：freeSpans_ 保持已申请未使用的 Span，减少系统调用
 *   5. 内存水位线：缓存超 128MB 时从大 Span 开始 munmap 归还 OS
 *
 * ── 为什么用 mmap 而不是 malloc？ ──
 *   因为我们的目标是替换 malloc。如果 PageCache 内部用 malloc，
 *   而用户代码把 malloc 替换成 MemoryPool::allocate，就会无限递归：
 *     MemoryPool → PageCache::systemAlloc → malloc → MemoryPool → ...
 *   使用 mmap 直接从内核分配物理页，彻底切断循环依赖。
 */

#include "PageCache.h"
#include <sys/mman.h>   // mmap, munmap, MAP_FAILED
#include <cstring>      // memset

namespace Kama_memoryPool
{

// =========================================================================
// allocateSpan —— 分配 numPages 页的连续内存
// =========================================================================
//
// CentralCache 唯一的上货入口。使用 Best-Fit 策略在 freeSpans_ 中查找。
//
// 流程：
//   1. 获取全局互斥锁
//   2. lower_bound 查找 ≥ numPages 的最小空闲 Span
//      分支 A（找到）：从 freeSpans_ 摘除 → 扣 cachedPages_
//                    → 大了就切分，剩余放回 → 加 cachedPages_
//      分支 B（没找到）：systemAlloc → mmap 新内存
//   3. 记录 spanMap_（地址→Span 反查，供释放使用）
//   4. 返回起始地址
//
// Best-Fit 示例：
//   freeSpans_ = {1页, 3页, 5页, 10页}
//   请求 4 页 → lower_bound(4) → 5 页条目 → 切分：3 页返回 + 2 页缓存
//   请求 5 页 → lower_bound(5) → 5 页条目 → 精确匹配，直接返回

void* PageCache::allocateSpan(size_t numPages)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // ---- 在 freeSpans_ 中查找 Best-Fit ----
    auto it = freeSpans_.lower_bound(numPages);

    if (it != freeSpans_.end())
    {
        // ============================================================
        // 分支 A：找到合适空闲 Span
        // ============================================================
        Span* span = it->second;

        // 从缓存计数中扣除当前 Span 的页数
        cachedPages_ -= span->numPages;

        // 从 freeSpans_ 链表中摘除头节点
        if (span->next)
        {
            freeSpans_[it->first] = span->next;
        }
        else
        {
            freeSpans_.erase(it); // 链表唯一节点，删除整个条目
        }

        // 如果 Span 比需求大，切分：前 numPages 页返回，剩余放回缓存
        if (span->numPages > numPages)
        {
            Span* newSpan = new Span;
            // 剩余部分从 "当前地址 + numPages×4096" 开始
            newSpan->pageAddr = static_cast<char*>(span->pageAddr)
                                + numPages * PAGE_SIZE;
            newSpan->numPages = span->numPages - numPages;
            newSpan->next = nullptr;

            // 剩余部分头插法放回 freeSpans_
            auto& list = freeSpans_[newSpan->numPages];
            newSpan->next = list;
            list = newSpan;

            // 剩余部分回到缓存，计数加回
            cachedPages_ += newSpan->numPages;

            // 返回部分的页数更新为请求值
            span->numPages = numPages;
        }

        // 记录地址→Span 映射，供 deallocateSpan 反查
        spanMap_[span->pageAddr] = span;
        return span->pageAddr;
    }

    // ============================================================
    // 分支 B：没有够大的空闲 Span → mmap 新内存
    // ============================================================
    void* memory = systemAlloc(numPages);
    if (!memory) return nullptr;

    // 创建 Span 控制块（堆分配，不在 mmap 区域内）
    Span* span = new Span;
    span->pageAddr = memory;
    span->numPages = numPages;
    span->next = nullptr;

    // 记录映射（不加入 freeSpans_，因为正在被使用）
    spanMap_[memory] = span;
    return memory;
}

// =========================================================================
// deallocateSpan —— 释放 Span 回 PageCache
// =========================================================================
//
// CentralCache 发现某 Span 全空闲时调用。流程：
//   1. spanMap_ 反查 Span 控制块
//   2. 后向合并：检查紧接在后面的 Span 是否空闲，是则合并
//   3. 插入 freeSpans_（头插法）
//   4. 超水位线 → releaseExcessSpans → munmap 归还 OS
//
// 修复 #05：新增 cachedPages_ 计数和水位线检查，
// 防止长期运行进程内存只增不减。

void PageCache::deallocateSpan(void* ptr, size_t numPages)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 反查 Span 控制块
    auto it = spanMap_.find(ptr);
    if (it == spanMap_.end()) return; // 不是我们分配的

    Span* span = it->second;

    // ---- 后向合并 ----
    // 检查紧接在当前 Span 后面的地址是否在 spanMap_ 中
    void* nextAddr = static_cast<char*>(ptr) + numPages * PAGE_SIZE;
    auto nextIt = spanMap_.find(nextAddr);

    if (nextIt != spanMap_.end())
    {
        Span* nextSpan = nextIt->second;

        // 验证 nextSpan 确实空闲（在 freeSpans_ 链表中）
        bool found = false;
        auto& nextList = freeSpans_[nextSpan->numPages];

        if (nextList == nextSpan)
        {
            nextList = nextSpan->next; // 头节点，直接摘除
            found = true;
        }
        else if (nextList)
        {
            // 在链表中搜索
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

        if (found)
        {
            // 合并：扩大当前 Span 的页数，删除 nextSpan
            span->numPages += nextSpan->numPages;
            spanMap_.erase(nextAddr);
            delete nextSpan;
        }
    }

    // ---- 插入 freeSpans_ 空闲链表（头插法）----
    auto& list = freeSpans_[span->numPages];
    span->next = list;
    list = span;

    // 更新缓存页数计数
    cachedPages_ += span->numPages;

    // ---- 内存水位线检查 ----
    // 超过 128MB 阈值，从大 Span 开始 munmap 归还 OS
    if (cachedPages_ > MAX_CACHED_PAGES)
    {
        releaseExcessSpans();
    }
}

// =========================================================================
// releaseExcessSpans —— 缓存超阈值时释放多余 Span 归还 OS
// =========================================================================
//
// 触发条件：cachedPages_ > MAX_CACHED_PAGES (128MB)
// 释放目标：降到 MAX_CACHED_PAGES / 2 (64MB)
// 策略：从大 Span 开始释放（大 Span 灵活性低，优先归还）
//
// 修复 #05：之前无此机制，长期运行进程内存只增不减。

void PageCache::releaseExcessSpans()
{
    size_t targetPages = MAX_CACHED_PAGES / 2; // 降到 64MB

    // 从最大页数开始（reverse_iterator），优先释放大 Span
    for (auto it = freeSpans_.rbegin();
         it != freeSpans_.rend() && cachedPages_ > targetPages; )
    {
        Span* span = it->second;
        while (span && cachedPages_ > targetPages)
        {
            Span* next = span->next;

            // 真正归还物理内存给 OS
            munmap(span->pageAddr, span->numPages * PAGE_SIZE);

            // 清理映射
            spanMap_.erase(span->pageAddr);
            cachedPages_ -= span->numPages;
            delete span; // 释放控制块

            span = next;
        }

        it->second = span; // 更新链表头

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

// =========================================================================
// systemAlloc —— 向 OS 申请 numPages 页连续内存
// =========================================================================
//
// 整个项目中唯一直接与 OS 交互的函数。使用 mmap 绕过 malloc。
//
// mmap 参数：
//   MAP_PRIVATE | MAP_ANONYMOUS → 私有匿名映射，不关联文件
//   PROT_READ | PROT_WRITE       → 可读可写
//   addr = nullptr               → 内核选择地址
//
// 显式 memset 清零：
//   强制内核立即分配物理页（触发缺页中断），避免用时延迟。
//   虽然 MAP_ANONYMOUS 默认给零页，但惰性分配会导致首次访问时的缺页开销。

void* PageCache::systemAlloc(size_t numPages)
{
    size_t size = numPages * PAGE_SIZE;

    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (ptr == MAP_FAILED) return nullptr;

    memset(ptr, 0, size); // 显式清零，触发物理页分配
    return ptr;
}

} // namespace Kama_memoryPool
