# 内存池代码 Review 报告

- 日期：2026-08-12
- 范围：`include/` + `src/` 全部核心代码（ThreadCache / CentralCache / PageCache / Common），另含测试文件
- 结论：**存在 2 个致命问题（P0）、4 个严重问题（P1）、若干一般问题（P2）**。单元测试当前全部通过，但测试规模（活跃内存 < 32MB）无法触发 P0 问题，掩盖了缺陷。

---

## 一、致命问题（P0）——必须修复

### P0-1：`expandTrackerArray` 排序数组后，新 Span 覆盖正在使用的 SpanTracker ⭐⭐⭐⭐⭐

**这是"二分写的不对"的根因**（详见第四节）。

**问题**：SpanTracker 的索引语义有两套，代码混用了：

- 创建序：`trackerIndex = spanCount_++`，第 N 个创建的 Span 用数组下标 N；
- 地址序：`expandTrackerArray` 和 `ensureSorted` 把数组按 `spanAddr` 排序，供二分查找。

`expandTrackerArray` 扩容时把**整个数组（含刚 new 出来的空槽）**排序。排序后空槽（`spanAddr=nullptr`）全部排到数组**最前面**，活跃 tracker 全部排在后面；此时 `array[trackerIndex]`（创建序下标）落在**活跃区中间**，新 Span 的数据（`spanAddr`/`numPages`/`blockCount`/`freeCount`）会**覆盖一个正在使用的 SpanTracker**。同时 `sortedCount_` 被设为含空槽的 `newSize`，大于 `spanCount_`（P1-1 的来源）。

**触发条件**：`spanCount_` 达到 1024（约 32MB 活跃内存）首次扩容后，**此后每次分配新 Span 都覆盖一个活跃 tracker**，直到数组再次满、再次扩容，雪崩式破坏。

**后果**：
1. 被覆盖的旧 Span 在数组中"消失" → `getSpanTracker` 永远找不到它 → `performDelayedReturn` 无法判定它全空闲 → **该 Span 永不归还 PageCache，永久内存泄漏**；
2. `spanCount_`、`sortedCount_`、数组实际内容三者语义彻底错乱，二分查找在有"空洞"的数组上漏查；
3. 新加的空槽永远不会被用到（索引永远落在活跃区）。

> 已修复（方案 A），完整记录见 `15-expandTrackerArray扩容排序覆盖活跃SpanTracker.md`

---

### P0-2：Span 归还后 SpanTracker 未失效（僵尸 tracker），地址复用时二分误命中 → 潜在 UAF ⭐⭐⭐⭐⭐

**位置**：`src/CentralCache.cpp:456-502`（`updateSpanFreeCount`）、`:535-578`（`getSpanTracker`）

**问题**：Span 全空闲归还 PageCache 时（`:500`），**只调用了 `PageCache::deallocateSpan`，没有清除该 tracker 的 `spanAddr`**。归还的地址进入 PageCache 的 `freeSpans_` 缓存。

而 PageCache 采用 Best-Fit（`PageCache.cpp:62`），**同一大小的 Span 几乎必然复用刚归还的地址**。于是：

- 旧 Span（地址 X）归还 → 僵尸 tracker A 残留（`spanAddr = X`，`freeCount = blockCount`）
- 地址 X 再次分配 → 新 tracker B（`spanAddr = X`）
- 数组中 A、B 的地址区间**完全重叠**

`getSpanTracker` 查找地址 X 的块时，二分/线性扫描命中 A 还是 B **取决于排序后 A、B 的相对位置（std::sort 对相同键不稳定）——约 50% 概率命中僵尸 A**：

1. 命中 A：`scanCount` 累加在 A 上 → `updateSpanFreeCount(A, count)`，若 `count == A.blockCount`（同大小 Span 时几乎必然成立）→ **把地址 X 再次归还 PageCache**——而此时 X 是 B 正在使用的内存 → 被 munmap 或并入 freeSpans_ → **悬垂指针 / Use-After-Free / 双归还**；
2. 命中 A 而 count 不等：B 的 `freeCount` 永不更新 → B 永不归还 → 泄漏。

**触发条件**：任意"Span 全空闲归还 → 同地址重新分配"循环，在稳定工作负载下**必然发生**。

> 已修复：归还 PageCache 后桶锁内 `spanAddr.store(nullptr)` + `freeCount.store(0)`，完整记录见 `16-Span归还后SpanTracker未失效.md`。僵尸槽位累积问题另记 P2-11。

---

## 二、严重问题（P1）——建议修复

### P1-1：`sortedCount_` 被设为含空槽的 `newSize`，与 `spanCount_` 语义不一致

**位置**：`src/CentralCache.cpp:623`

`expandTrackerArray` 排序了 `[0, newSize)`（含空槽）并把 `sortedCount_` 设为 `newSize`，而 `spanCount_`（实际使用的条目数）小于它。二分区间 `[0, sorted)` 包含 nullptr 前缀：

- 二分循环体对空槽的跳过逻辑（`blockAddr >= addr + 0` 恒真 → 右移）恰好安全，**单独看无害**；
- 但它掩盖了 P0-1 的后果：此后 `sortedCount_ > spanCount_` 时，阶段 2 线性扫描 `for (i = sorted; i < total; ++i)`（`:565`）因 `sorted > total` **永不执行**，二分一旦漏查就彻底查不到——P0-1 的放大器。

已随 P0-1 修复（方案 A）消失：`sortedCount_` 恒 `<= spanCount_`。

### P1-2：并发下 `trackerArray_` 与 `sortedCount_` 的组合读取窗口

**位置**：`src/CentralCache.cpp:537-539`（读）、`:621-623`（写）、`:669-670`（写）

不同桶锁的线程会并发读写 tracker 数组（A 持桶锁 X 执行 `expandTrackerArray`/`ensureSorted`，B 持桶锁 Y 执行 `getSpanTracker`）。写方先 store `array` 再 store `sortedCount`，读方先 load `array` 再 load `sortedCount`，可能读到"新数组 + 旧 sortedCount"的组合：

- 新数组 `[0, newSorted)` 已排序，二分在 `[0, 旧sorted)` 子区间查找，**不会越界**（旧sorted ≤ newSorted ≤ 数组容量）；
- 若目标 Span 恰好在新加入的排序区，二分 miss，此时线性扫描 `[sorted, total)` 用旧 sorted → 扫描范围扩大，**能兜底找到**。

结论：**当前顺序下"新数组 + 旧 sorted"组合实际无害**（漏查被线性扫描兜底），但非常脆弱。**注意：review 未覆盖"旧数组 + 新 sorted"组合——二分在旧数组未排序区漏查且线性扫描兜底不到，导致 Span 永不归还（泄漏）**；另有 `spanCount_` 先于扩容递增造成的"旧数组 + 新 total"越界窗口。

> 已修复：写序契约 + 读者双读校验（seqlock 模式）+ 上界截断 `min(total, capacity)`，完整记录见 `17-并发下trackerArray与sortedCount组合读取窗口.md`。

### P1-3：PageCache 切分后的剩余 Span 未登记 `spanMap_`，后向合并失效

**位置**：`src/PageCache.cpp:85-104`（切分）

```cpp
if (span->numPages > numPages)
{
    Span* newSpan = new Span;
    newSpan->pageAddr = span->pageAddr + numPages * PAGE_SIZE;
    ...
    freeSpans_[newSpan->numPages] 头插放回缓存   // ← 只进了 freeSpans_
    ...
}
spanMap_[span->pageAddr] = span;                 // ← 只登记了返回的前半部分
```

`newSpan` 放回 `freeSpans_` 但**没有登记 `spanMap_`**。`deallocateSpan` 的后向合并靠 `spanMap_.find(nextAddr)`（`:153-154`）定位相邻 Span，因此：

- 切分出的剩余部分**从未被分配过**就遇到相邻归还时，无法被合并；
- 只有等它自己被分配、归还过一次（此时才进 `spanMap_`）后，合并才恢复工作。

**后果**：切分场景下外部碎片无法完全对抗，长期运行内存膨胀。

> 已修复：切分时 `spanMap_[newSpan->pageAddr] = newSpan`，完整记录见 `18-PageCache切分剩余Span未登记spanMap.md`

### P1-4：`deallocateSpan` 用 `operator[]` 访问 `freeSpans_`，对"在用"的相邻 Span 产生空条目

**位置**：`src/PageCache.cpp:162`

```cpp
auto& nextList = freeSpans_[nextSpan->numPages];  // operator[]，若 nextSpan 在用则插入空条目！
```

当 `nextSpan` 在 spanMap_ 中但**不在 freeSpans_**（正在被使用）时，`operator[]` 会向 map 插入 `numPages → nullptr` 的空条目。虽然 `releaseExcessSpans` 能清理（`:246-251`），但 map 会无谓膨胀；且 `allocateSpan` 的 `lower_bound` 命中空条目时解引用 nullptr 会崩溃。

> 已修复：`operator[]` → `find`，完整记录见 `19-deallocateSpan的operator[]插入空条目.md`

---

## 三、一般问题（P2）

| # | 位置 | 问题 |
|---|------|------|
| 1 | `PageCache.cpp:57-126` | `allocateSpan` 复用路径**不重新清零**，与头文件注释"已清零"（`PageCache.h:47`）矛盾。小块场景靠用户自写覆盖，但大块（>256KB 直走 malloc 之外的块）语义不符文档 |
| 2 | `CentralCache.cpp:327-342` | `returnRange` 按 `count < blockCount` 遍历（`:330`），若 ThreadCache 传入链表实际长度 > `blockCount`（计数器偏差），`*end = current`（`:341`）会切断并**丢失多余块**。计数器自洽时不会发生，但缺少防御 |
| 3 | `PageCache.cpp:141-208` | 只有**后向合并**，无前向合并。相邻两 Span 中先归还后面那个时，前面的无法合并（设计限制，但值得记录） |
| 4 | `CentralCache.cpp:395-442` | `performDelayedReturn` 在桶锁内调用 `PageCache::deallocateSpan`（可能 munmap、可能毫秒级），长临界区放大锁竞争；同样 `fetchRange` 分支 A 在桶锁内 new + mmap |
| 5 | `CentralCache.cpp:549-556` | `blockAddr < addr`、`blockAddr >= addr + len` 这类**跨对象指针关系比较**，严格按 C++ 标准是未定义行为（仅同一数组/对象的比较有定义）。x86 + glibc 实践中按地址数值比较、正常工作，但启用激进优化（`-fstrict-aliasing` 类）时需注意；建议改为 `uintptr_t` 比较 |
| 6 | `ThreadCache.cpp:113`、`CentralCache.h:136-143` | 阈值硬编码（256 / BATCH_SIZE=8 / SORT_THRESHOLD=64 / MAX_DELAY_COUNT=48 / SPAN_PAGES=8），文档 #09 已提 |
| 7 | `CentralCache.cpp:587-624` | 扩容产生的旧 `trackerArray_` 指针数组**永不释放**（每次 +256 指针，进程生命周期内累积，量级可忽略但需记录） |
| 8 | `ThreadCache.cpp:159-201` | `returnToCentralCache` 的 `actualReturn` 基于计数器而非实际链表遍历（`:190`），与文档 #12 的场景互为镜像；计数器自洽时正确，异常时依赖 CentralCache 侧防御 |
| 9 | `tests/UnitTest.cpp:71` | 多线程共享 `rand()`（glibc 内部加锁，安全但慢）；未播种 `srand`，测试可复现但覆盖固定 |
| 10 | `ensureSorted` | ~~`ensureSorted` 中两个复制循环冗余（等价于复制整个 `[0, arraySize)`），可合并~~ ✅ 已随 P1-2 修复合并 |

---

## 四、二分查找专项分析（你重点怀疑的地方）

### `getSpanTracker` 的二分循环体本身是正确的

```cpp
// CentralCache.cpp:542-562
size_t left = 0, right = sorted;
while (left < right)
{
    size_t mid = left + (right - left) / 2;
    ...
    if (blockAddr < addr)                right = mid;      // 目标在左区间
    else if (blockAddr >= addr + len)    left = mid + 1;   // 目标在右区间
    else                                 return t;         // 命中
}
```

在"数组 `[0, sorted)` 内按 `spanAddr` 升序、区间两两不重叠"的前提下，这是标准的区间二分，逻辑无误（`>=` 处理了区间右开边界）。

### 但二分的前提被两处破坏

1. **`expandTrackerArray` 排序整个数组（含空槽）并把 `sortedCount_` 设为 `newSize`**（P0-1，已修复见 `15-expandTrackerArray扩容排序覆盖活跃SpanTracker.md`）：排序后数组索引从"创建序"变成"地址序"，而分配方继续用 `spanCount_++` 作索引 → 覆盖活跃 tracker；`sortedCount_ > spanCount_`，数组出现"空洞"，二分查找的是**不完整、被篡改**的数组 → 漏查。**排序范围、`sortedCount_` 设置、索引使用方式三者互相矛盾。**

2. **僵尸 tracker 地址复用**（P0-2）：数组中两个 tracker 指向同一地址区间，二分命中哪个完全随机，命中僵尸即灾难。

### 二分正确的场景下性能也有隐患

`fetchRange` 分支 B（`:268-291`）每次取块都做一次 O(log n) 二分 + 可能 O(64) 线性扫描（未排序尾部），在热路径上；每块一次 `getSpanTracker` 调用（`:284-290` 循环内）。相比直接遍历链表统计，属于可接受的权衡，但值得记录。

---

## 五、修复建议汇总

| 优先级 | 问题 | 建议修复 |
|--------|------|----------|
| P0-1 | expand 排序破坏索引 | ✅ 已修复（方案 A）——`15-expandTrackerArray扩容排序覆盖活跃SpanTracker.md` |
| P0-2 | 僵尸 tracker | ✅ 已修复——`16-Span归还后SpanTracker未失效.md` |
| P1-1 | sortedCount 含空槽 | 随 P0-1 修复消失（保证 `sortedCount_ <= spanCount_`） |
| P1-2 | 并发读组合窗口 | ✅ 已修复——`17-并发下trackerArray与sortedCount组合读取窗口.md` |
| P1-3 | 切分剩余未登记 | ✅ 已修复——`18-PageCache切分剩余Span未登记spanMap.md` |
| P1-4 | operator[] 空条目 | ✅ 已修复——`19-deallocateSpan的operator[]插入空条目.md` |
| P2 | 其余 | 见第三节 |

### P0-1 修复状态

已按方案 A 实施（2026-08-18）：`expandTrackerArray` 不再排序、`sortedCount_` 保持旧值，排序统一由 `ensureSorted` 对 `[0, spanCount_)` 执行。完整记录见 `15-expandTrackerArray扩容排序覆盖活跃SpanTracker.md`。

---

## 六、测试覆盖缺口

| 缺口 | 说明 |
|------|------|
| 未覆盖扩容路径 | P0-1 需 `spanCount_ > 1024`（约 32MB 活跃内存）才触发，单元测试总量远低于此 |
| 未覆盖"归还-复用"循环 | P0-2 需同地址 Span 归还后重新分配，测试随机释放后即结束，没有"分配→全归还→再分配同大小"的稳态循环 |
| 未覆盖大块（blockNum==1）| `blockNum==1` 分支（`:234-258`）测试中没有专门用例 |
| 未断言内存归还 | 测试只断言分配非空和写入正确，**没有断言 PageCache 侧内存回到缓存/OS**（如观测 RSS、或对 tracker 数量做内部断言） |
| 未验证 256KB 边界往返 | `allocate(MAX_BYTES)` 与 `allocate(MAX_BYTES+1)` 均通过，但无压力下的混合场景 |

建议补充：
1. 循环分配/释放同大小内存（稳态），运行数分钟，观测内存不增长（能抓到 P0-1/P0-2）；
2. 增加 2000+ 个 Span 的分配压力（能触发扩容路径）；
3. 对 tracker 状态做白盒断言：`getSpanTracker` 对每个已分配块都必须命中且唯一命中。
