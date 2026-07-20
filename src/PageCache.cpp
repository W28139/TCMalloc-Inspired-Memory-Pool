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
 *   [PageCache]      ← 全局共享，互斥锁，页级管理 ← 你在这里
 *        ↕
 *   [OS Kernel]      ← mmap / munmap 系统调用
 *
 * ── 核心职责 ──
 *
 *   1. 以"页"（PAGE_SIZE = 4096B）为单位，向操作系统申请和归还连续内存
 *   2. 将连续多页封装为 Span，对外提供 allocateSpan / deallocateSpan
 *   3. Span 切分：大 Span 可以切出一部分给上层，剩余部分缓存复用
 *   4. Span 合并：归还时检查相邻 Span，空闲则合并，对抗外部碎片
 *   5. 缓存管理：freeSpans_ 保持已申请但未使用的 Span，减少系统调用
 *
 * ── 核心数据结构 ──
 *
 *   struct Span {
 *       void*  pageAddr;  // Span 的起始地址（页对齐）
 *       size_t numPages;  // 包含的页数
 *       Span*  next;      // 同一页数链表中的下一个 Span
 *   };
 *
 *   freeSpans_:  std::map<size_t, Span*>
 *       key   = 页数 (1, 2, 3, 5, 8, ...)
 *       value = 该页数的空闲 Span 链表头
 *       例：freeSpans_[3] → Span(3页) → Span(3页) → nullptr
 *
 *   spanMap_:  std::map<void*, Span*>
 *       key   = Span 的起始地址
 *       value = 对应的 Span* 控制块
 *       用于 deallocateSpan 时快速找到 Span 元信息
 *
 * ── 设计要点 ──
 *
 *   Q: 为什么用 std::mutex 而不是自旋锁？
 *   A: PageCache 的操作可能包含 mmap 系统调用（毫秒级），
 *      让等待的线程在内核态挂起比在用户态空转更高效。
 *
 *   Q: Span* 控制块存在哪？
 *   A: 用 new/delete 分配在堆上。注意这里有一个微妙的"C++ 堆分配 vs 内存池"
 *      的问题 —— Span 控制块很小（24 字节），且数量有限（通常 < 1000），
 *      直接用 new/delete 不会造成性能问题。mmap 返回的连续内存只存用户数据。
 */

#include "PageCache.h"
#include <sys/mman.h>  // mmap, munmap, MAP_FAILED
#include <cstring>     // memset

namespace Kama_memoryPool
{

// =========================================================================
// allocateSpan —— 分配 numPages 页的连续内存
// =========================================================================
//
// 这是 PageCache 唯一的"出货"入口。CentralCache 调用它来获取新的 Span。
//
// 参数：
//   numPages - 需要的页数。对于 ≤32KB 的大小类别，CentralCache 传 8；
//              对于更大的，按实际需要向上取整
//
// 返回值：
//   void* 指向连续内存的起始地址（至少 numPages * 4096 字节）
//   失败返回 nullptr
//
// 查找策略：Best-Fit（最佳匹配）
//   从 freeSpans_ 中找到 ≥ numPages 的最小空闲 Span，
//   如果找不到，调用 systemAlloc 向 OS 申请新内存。
//
// 流程概览：
//   ┌─ 获取全局互斥锁
//   ├─ lower_bound 查找 ≥ numPages 的最小空闲 Span
//   │   ├─ 找到 → 从空闲链表取出
//   │   │        ├─ 大小刚好 → 直接返回
//   │   │        └─ 比要求大 → 切分，返回前面部分，剩余放回 freeSpans_
//   │   └─ 没找到 → systemAlloc → 新建 Span 控制块 → 返回
//   └─ 解锁，返回地址

void* PageCache::allocateSpan(size_t numPages)
{
    // ---- 第 1 步：获取全局互斥锁 ----
    // lock_guard 是 RAII 风格：构造时加锁，析构时（离开作用域）自动解锁
    // PageCache 只有这一把全局锁 —— 因为 PageCache 的调用频率很低
    // （只在 CentralCache 完全没有对应大小的空闲块时才触发）
    std::lock_guard<std::mutex> lock(mutex_);

    // ---- 第 2 步：在 freeSpans_ 中查找最佳匹配的空闲 Span ----
    //
    // lower_bound(key)：
    //   返回第一个 key ≥ numPages 的元素的迭代器
    //   如果所有 key 都 < numPages，返回 end()
    //
    //   例：freeSpans_ 中有 {1页, 3页, 5页, 10页} 四个条目
    //       请求 4 页 → lower_bound(4) → 返回 5 页的条目（最佳匹配）
    //       请求 5 页 → lower_bound(5) → 返回 5 页的条目（精确匹配）
    //       请求 20页 → lower_bound(20) → 返回 end()（没有够大的）
    auto it = freeSpans_.lower_bound(numPages);

    if (it != freeSpans_.end())
    {
        // ============================================================
        // 分支 A：找到了 ≥ numPages 的空闲 Span
        // ============================================================

        // it->first  = 该条目对应的页数（可能 > numPages）
        // it->second = 该页数的 Span 链表头指针
        Span* span = it->second;

        // --- A.1 将取出的 Span 从 freeSpans_ 中移除 ---
        //
        // freeSpans_[it->first] 是一个链表，span 是链表头
        // 如果链表有多个节点：头指针移到 span->next
        // 如果链表只有这一个节点：整个条目从 map 中删除
        if (span->next)
        {
            // 链表不止一个 Span：链表头移到下一个
            freeSpans_[it->first] = span->next;
        }
        else
        {
            // 链表仅此一个 Span：删除整个条目
            // 此时这个页数在 map 中不存在了，
            // 直到未来有同页数的 Span 被释放回来才会重新出现
            freeSpans_.erase(it);
        }

        // --- A.2 如果 Span 比需要的更大 → 切分 ---
        //
        // 为什么要切分？
        //   假设请求 3 页，但找到的是 10 页的空闲 Span。
        //   如果直接给 10 页，浪费了 7 页，上层也只用到前 3 页。
        //   切分后：前 3 页返回给调用者，后 7 页放回 freeSpans_ 等待下次使用。
        //
        // 切分示意图：
        //
        //   切分前： 10 页 Span
        //   [P0][P1][P2][P3][P4][P5][P6][P7][P8][P9]
        //    ↑ span->pageAddr
        //
        //   请求 3 页，切分后：
        //   返回给调用者： [P0][P1][P2]              (3 页，span->numPages=3)
        //   放回 freeSpans_：        [P3][P4]...[P9]  (7 页，newSpan)
        //                            ↑ newSpan->pageAddr = span->pageAddr + 3*4096
        //
        if (span->numPages > numPages)
        {
            // 创建一个新的 Span 控制块来描述剩余部分
            Span* newSpan = new Span;

            // newSpan 的起始地址 = 被取走部分结束的位置
            // 例：span 从 0x10000 开始，取走 3 页（3×4096=12288=0x3000）
            //     newSpan->pageAddr = 0x10000 + 0x3000 = 0x13000
            newSpan->pageAddr = static_cast<char*>(span->pageAddr)
                                + numPages * PAGE_SIZE;

            // newSpan 的页数 = 原来的总页数 - 被取走的页数
            newSpan->numPages = span->numPages - numPages;
            newSpan->next = nullptr;

            // --- 将剩余部分插入 freeSpans_ ---
            //
            // 插入到相应页数的链表头部（头插法）
            // & 引用语法：auto& list 是 freeSpans_[newSpan->numPages] 的引用，
            // 修改 list 就是直接修改 freeSpans_ 中的值
            auto& list = freeSpans_[newSpan->numPages];

            // 头插法：newSpan 放在链表最前面
            // 合并前：list → oldHead → ...
            // 合并后：list → newSpan → oldHead → ...
            newSpan->next = list;
            list = newSpan;

            // 更新被取走部分的页数为请求的页数
            span->numPages = numPages;
        }

        // --- A.3 记录到 spanMap_ 供后续回收使用 ---
        //
        // 为什么要 spanMap_？
        //   回收时（deallocateSpan），上层只知道一个 void* 地址，
        //   需要通过这个 map 反查到对应的 Span 控制块，
        //   从而获取页数、执行合并等操作。
        spanMap_[span->pageAddr] = span;

        // 返回 Span 的起始地址（页对齐的 void* 指针）
        // 注意：Span* 控制块本身不在这块连续内存中，
        // 它是用 new 在堆上单独分配的
        return span->pageAddr;
    }

    // ============================================================
    // 分支 B：freeSpans_ 中没有足够大的空闲 Span → 向 OS 申请新内存
    // ============================================================

    // --- B.1 通过 mmap 向操作系统申请 numPages 页的连续内存 ---
    void* memory = systemAlloc(numPages);
    if (!memory) return nullptr;  // 系统内存耗尽

    // --- B.2 创建 Span 控制块 ---
    // Span 控制块用 new 分配在堆上，不在 mmap 返回的内存中
    Span* span = new Span;
    span->pageAddr = memory;      // 记录起始地址
    span->numPages = numPages;    // 记录页数
    span->next = nullptr;         // 尚未在 freeSpans_ 中，next 暂空

    // --- B.3 记录到 spanMap_ 供回收时反查 ---
    spanMap_[memory] = span;

    return memory;
}

// =========================================================================
// deallocateSpan —— 释放（回收）一个 Span
// =========================================================================
//
// 这是 PageCache 的"回收"入口。CentralCache 在发现某个 Span 的所有小块
// 都已归还时调用此函数，将整 Span 还给 PageCache。
//
// 参数：
//   ptr      - Span 的起始地址（必须与 allocateSpan 返回的地址一致）
//   numPages - Span 包含的页数
//
// 核心逻辑：
//   1. 通过 spanMap_ 反查 Span 控制块
//   2. 尝试向后合并相邻空闲 Span
//   3. 将（合并后的）Span 插入 freeSpans_，等待下次分配复用
//
// 注意：当前实现只做后向合并（检查紧接在后面的 Span），
// 不做前向合并。真正的 TCMalloc 会同时检查前后两个方向。

void PageCache::deallocateSpan(void* ptr, size_t numPages)
{
    // ---- 第 1 步：获取全局互斥锁 ----
    std::lock_guard<std::mutex> lock(mutex_);

    // ---- 第 2 步：反查 Span 控制块 ----
    //
    // spanMap_ 的 key 是 Span 的起始地址，
    // 如果找不到，说明这块内存不是 PageCache 分配的
    // （可能是大对象直接走 malloc 的，或者是错误调用）
    auto it = spanMap_.find(ptr);
    if (it == spanMap_.end()) return;  // 不是我们的，不管

    Span* span = it->second;  // 取出 Span 控制块

    // ---- 第 3 步：尝试后向合并 ----
    //
    // 后向合并：检查紧接在"当前 Span 结束地址"后面是否有一个空闲 Span
    //
    //   当前 Span:   [P0][P1][P2]           (地址 0x1000, 3页)
    //   nextAddr:                           0x1000 + 3×4096 = 0x4000
    //   如果 spanMap_ 中存在 key=0x4000 的 Span → 可以合并!
    //
    // 为什么只做后向合并？
    //   前向合并（检查前面的 Span）需要知道前一个 Span 的大小，
    //   通过 spanMap_ 用地址反查不容易做到（key 是起始地址，不是结束地址）。
    //   真正的 TCMalloc 通过基数树（Radix Tree）来快速查找任意页所属的 Span，
    //   从而支持前后双向合并。当前项目用 std::map 简化实现，只做后向合并。
    //
    // 合并的安全条件：
    //   相邻 Span 必须确实在 freSpans_ 空闲列表中 ——
    //   如果它正在被 CentralCache 使用，绝对不能合并！
    //   代码通过遍历 freeSpans_ 链表来验证这一点。

    // 计算紧接在当前 Span 后面的地址
    // static_cast<char*> 是为了以字节为单位做指针运算
    void* nextAddr = static_cast<char*>(ptr) + numPages * PAGE_SIZE;

    // 在 spanMap_ 中查找起始地址正好等于 nextAddr 的 Span
    auto nextIt = spanMap_.find(nextAddr);

    if (nextIt != spanMap_.end())
    {
        // 存在地址相邻的 Span
        Span* nextSpan = nextIt->second;

        // --- 3.1 验证 nextSpan 是否在 freeSpans_ 空闲链表中 ---
        //
        // 在 spanMap_ 中存在 ≠ 是空闲的。spanMap_ 记录了所有
        // PageCache 分配过的 Span（包括正在被 CentralCache 使用的）。
        // 只有真正在 freeSpans_ 中的才是空闲的、可以合并的。
        //
        // 查找方式：在 nextSpan->numPages 对应的空闲链表中
        // 搜索 nextSpan 这个节点
        bool found = false;

        // & 引用：直接操作 map 中对应页数的链表
        auto& nextList = freeSpans_[nextSpan->numPages];

        // 情况 1：nextSpan 是链表头
        if (nextList == nextSpan)
        {
            nextList = nextSpan->next;  // 头指针后移，摘除链表头
            found = true;
        }
        // 情况 2：nextSpan 在链表中间或尾部
        else if (nextList)  // 链表非空才需要遍历
        {
            Span* prev = nextList;
            while (prev->next)
            {
                if (prev->next == nextSpan)
                {
                    // 找到了！跳过 nextSpan 这个节点
                    // 合并前：prev → nextSpan → nextSpan->next → ...
                    // 合并后：prev → nextSpan->next → ...
                    prev->next = nextSpan->next;
                    found = true;
                    break;
                }
                prev = prev->next;
            }
        }

        // --- 3.2 只有在验证了是空闲的前提下才执行合并 ---
        if (found)
        {
            // 物理上不需要做任何事 —— 两块内存本来就是连续的！
            // 只需要更新 Span 控制块的元数据：
            //   - 扩大当前 Span 的页数
            //   - 从 spanMap_ 中删除 nextSpan 的条目
            //   - 删除 nextSpan 控制块（用 delete，因为它用 new 创建的）
            span->numPages += nextSpan->numPages;

            // 从 spanMap_ 中移除：合并后整个区域以 span->pageAddr 为 key
            spanMap_.erase(nextAddr);

            // 释放 nextSpan 控制块的内存
            delete nextSpan;
        }
    }

    // ---- 第 4 步：将（合并后的）Span 插入 freeSpans_ 空闲列表 ----
    //
    // 使用头插法，O(1) 时间插入链表头部
    //
    // 插入前：
    //   freeSpans_[span->numPages] → existingHead → ...
    //
    // 插入后：
    //   freeSpans_[span->numPages] → [span] → existingHead → ...
    //
    // auto& list 是 freeSpans_[span->numPages] 的引用
    auto& list = freeSpans_[span->numPages];
    span->next = list;   // span 的 next 指向旧链表头
    list = span;         // 链表头更新为 span

    // 注意：这里并没有调用 munmap 把内存还给操作系统！
    // Span 被缓存在 freeSpans_ 中，等待下次 allocateSpan 直接复用。
    // 这避免了频繁的 mmap/munmap 系统调用。
    //
    // 如果未来需要真正归还内存（如进程内存压力大时），
    // 可以在这里添加策略：如果 freeSpans_ 中缓存的页数超过某个阈值，
    // 就调 munmap 真正释放一部分。
}

// =========================================================================
// systemAlloc —— 向操作系统申请 numPages 页的连续内存
// =========================================================================
//
// 这是整个内存池项目中唯一直接与 OS 交互的函数。
// 它绕过了 C 运行时的 malloc/free，直接使用 Linux 的 mmap 系统调用。
//
// 为什么不用 malloc？
//   因为我们的目标是替换 malloc！如果 PageCache 内部用 malloc 来申请内存，
//   而用户代码又把 malloc 替换成 MemoryPool::allocate，就会形成：
//
//     MemoryPool::allocate → ... → PageCache::systemAlloc → malloc
//       → MemoryPool::allocate → ... → PageCache::systemAlloc → malloc
//         → ... 无限递归！
//
//   使用 mmap 直接从内核分配物理页，彻底切断这个循环依赖。
//
// 参数：
//   numPages - 页数（每页 PAGE_SIZE = 4096 字节）
//
// 返回值：
//   成功：指向清零后的连续内存的 void* 指针
//   失败：nullptr（系统内存耗尽）

void* PageCache::systemAlloc(size_t numPages)
{
    // 计算实际需要的字节数
    size_t size = numPages * PAGE_SIZE;

    // ---- mmap 系统调用 ----
    //
    // 参数详解：
    //
    //   addr   = nullptr
    //     让内核自己选择起始地址。内核会找到一块足够大的空闲虚拟地址空间。
    //
    //   length = numPages * 4096
    //     请求的内存大小，内核会向上取整到页的整数倍。
    //
    //   prot   = PROT_READ | PROT_WRITE
    //     内存保护标志：可读 + 可写。不需要 PROT_EXEC（不可执行），
    //     因为内存池只分配数据内存，不分配可执行代码。
    //
    //   flags  = MAP_PRIVATE | MAP_ANONYMOUS
    //     MAP_PRIVATE：  私有映射，对此内存的修改不会写回文件，
    //                    也不会被其他进程看到（COW — Copy On Write）。
    //     MAP_ANONYMOUS：匿名映射，不关联任何文件。
    //                    内存初始内容为 0（内核标记为零页，惰性分配）。
    //
    //   fd     = -1
    //     MAP_ANONYMOUS 时此参数被忽略。某些实现要求填 -1。
    //
    //   offset = 0
    //     无文件偏移（没有关联文件）。
    //
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    // mmap 失败时返回 MAP_FAILED（即 (void*)-1），不是 nullptr
    if (ptr == MAP_FAILED) return nullptr;

    // ---- 显式清零 ----
    //
    // 虽然 MAP_ANONYMOUS 的内存在首次访问时内核会提供零页，
    // 但显式 memset 有两个好处：
    //   1. 强制内核立即分配物理页（触发缺页中断），避免后续使用时才触发、
    //      在性能敏感路径上产生不可预期的延迟（页错误开销在微秒级）
    //   2. 防御性编程：如果将来改用其他分配方式（如 brk），
    //      保证行为一致
    memset(ptr, 0, size);

    return ptr;
}

} // namespace Kama_memoryPool
