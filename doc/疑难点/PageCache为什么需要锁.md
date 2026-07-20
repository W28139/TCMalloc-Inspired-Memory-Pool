# PageCache 为什么需要锁？

## 疑问

> CentralCache 已经有桶级自旋锁（`locks_[32768]`）了，而且 CentralCache 是唯一调用 PageCache 的地方，那 PageCache 里 `std::mutex mutex_` 是不是多余的？

## 关键认知纠正

CentralCache 的桶锁是**每个 index 一把**，不是**全局一把**。不同 index 的锁**互不阻塞**。

```
locks_[0] — 保护 8B  大小类的 centralFreeList_[0]
locks_[1] — 保护 16B 大小类的 centralFreeList_[1]
locks_[2] — 保护 24B 大小类的 centralFreeList_[2]
  ...
locks_[32767] — 保护 256KB 大小类的 centralFreeList_[32767]
```

线程 A 持有 `locks_[2]`，线程 B 仍然可以同时持有 `locks_[5]`。两者互不感知。

## 竞态场景

```
线程 A (index=2, 请求 24B 块):              线程 B (index=5, 请求 48B 块):
───────────────────────────────────        ──────────────────────────────────
locks_[2].test_and_set() ✅                 locks_[5].test_and_set() ✅
                                            centralFreeList_[5] 有货 ✅
                                              → 直接取一块返回
                                              → locks_[5].clear()
                                              → 线程 B 完成（不涉及 PageCache）

centralFreeList_[2] 为空 ❌
  → fetchFromPageCache(24)
    → PageCache::allocateSpan(8) ───────────── 只有线程 A 进入 PageCache
      → mutex_.lock()                          → 没有竞争，但这是运气好
```

上面这个场景**碰巧没撞**。换一个场景：

```
线程 A (index=2):                          线程 B (index=5):
locks_[2].test_and_set() ✅                 locks_[5].test_and_set() ✅
centralFreeList_[2] 为空 ❌                   centralFreeList_[5] 为空 ❌
  → fetchFromPageCache(24)                    → fetchFromPageCache(48)
    → allocateSpan(8) ────────┐                 → allocateSpan(8) ────────┐
                              ├── 同时进入 PageCache！                      │
                              │                                            │
       如果 PageCache 没有锁，两个线程同时：                                    │
                              │                                            │
       ① freeSpans_.lower_bound(8)                                         │
          → 线程 A 找到 10 页 Span                                          │
                              ② freeSpans_.lower_bound(8)                   │
                                 → 线程 B 也找到 同一个 10 页 Span！           │
                                   因为线程 A 还没来得及从 map 中移除它          │
                                                                           │
       ③ 线程 A: spanMap_[0x1000] = spanA                                  │
       ④ 线程 B: spanMap_[0x1000] = spanB  ← 覆盖！                         │
                                                                           │
       结果：同一个物理内存块被分配了两次 → 数据损坏                              │
```

## 锁的分层设计

这不是"多余的锁"，而是**分层锁设计**：

```
层级        锁类型           锁粒度           竞争概率
────────────────────────────────────────────────────
ThreadCache  无锁 (TLS)     每线程独立         0（物理隔离）
CentralCache 桶级自旋锁     每大小类一把       很低（只在同大小类竞争）
PageCache    全局互斥锁     唯一一把           极低（Cold Path）
```

| 特性 | CentralCache 自旋锁 | PageCache 互斥锁 |
|:---|:---|:---|
| 数量 | 32768 把 | 1 把 |
| 保护对象 | `centralFreeList_[i]` | `freeSpans_` + `spanMap_` |
| 锁类型 | 自旋（`atomic_flag`） | 挂起（`std::mutex`） |
| 临界区耗时 | ~几十 ns（取链表头指针） | ~μs~ms（可能含 mmap） |
| 调用频率 | 高（ThreadCache 每次没命中） | 极低（CentralCache 完全没缓存时） |

PageCache 的锁保护的是所有桶锁背后的**共享底层资源**——`freeSpans_` 和 `spanMap_` 这两个 `std::map`。虽然从外部看 CentralCache 是唯一调用者，但 CentralCache 内部有 32768 条路可以同时通到 PageCache。

## 简短总结

> 桶锁让不同大小的请求互相不阻塞，但一旦它们同时需要访问底层页管理，就必须在 PageCache 串行化。PageCache 的 `mutex_` 保护的不是"谁在调用"，而是"共享的 `std::map` 数据结构"。
