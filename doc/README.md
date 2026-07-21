# 文档索引

仿 TCMalloc 内存池项目的辅助文档。

---

## 目录结构

```
doc/
├── README.md
├── optimizations/   ← 所有可优化点（bug / 性能 / 设计 / 工程）
├── qa/              ← 学习过程中的 Q&A
└── benchmarks/      ← 压测报告，每次更新一份
```

---

## 优化项

按优先级排列，建议从上到下依次处理。

| 序号 | 文件 | 简述 | 类型 |
|:---|:---|:---|:---|
| 00 | [压测基线](optimizations/00-压测基线.md) | 修复前的性能快照，后续所有优化的对比基准 | 📊 基线 |
| 01 | [freeCount重复计数导致永不归还](optimizations/01-freeCount重复计数导致永不归还.md) | `updateSpanFreeCount` 累加导致 freeCount 虚高，Span 永不归还 | 🐛 Bug |
| 02 | [freeListSize无符号下溢](optimizations/02-freeListSize无符号下溢.md) | `size_t` 减到 0 以下回绕到 SIZE_MAX | 🐛 Bug |
| 03 | [spanTrackers溢出无保护](optimizations/03-spanTrackers溢出无保护.md) | 超 1024 个 Span 后静默跳过，内存泄漏 | 🐛 Bug |
| 04 | [批量传输](optimizations/04-批量传输.md) | fetchRange 每次只给 1 块 vs TCMalloc 批量传输。含修改方案 | ⚡ 性能 |
| 05 | [mmap未归还OS](optimizations/05-mmap未归还OS.md) | PageCache 只缓存不释放，长期进程内存只增不减 | 🛡️ 设计 |
| 06 | [getSpanTracker线性遍历](optimizations/06-getSpanTracker线性遍历.md) | O(n) 扫描 spanTrackers_，Span 多时变慢 | ⚡ 性能 |
| 07 | [performDelayedReturn中使用unordered_map](optimizations/07-performDelayedReturn中使用unordered_map.md) | 持自旋锁期间触发 `unordered_map` 堆分配 → 改用 SpanTracker.scanCount 两遍扫描 | ✅ 已修复 |
| 08 | [ThreadCache数组占用过大](optimizations/08-ThreadCache数组占用过大.md) | 每线程 512KB TLS，100 线程 = 50MB | 💾 内存 |
| 09 | [配置参数硬编码](optimizations/09-配置参数硬编码.md) | 阈值全部硬编码，无法运行时调整 | 🔧 工程 |
| 10 | [blockNum等于1时无SpanTracker](optimizations/10-blockNum等于1时无SpanTracker.md) | blockNum==1 时创建 SpanTracker，大块正确回收 | ✅ 已修复 |
| 11 | [PageCache中的new可能导致循环依赖](optimizations/11-PageCache中的new可能导致循环依赖.md) | `new Span` 本质是 malloc，若作为 malloc 替代品会死锁 | 🐛 隐患 |

---

## 疑难点

| 文件 | 简述 |
|:---|:---|
| [why-pagecache-needs-mutex](qa/PageCache为什么需要锁.md) | CentralCache 已有桶锁，PageCache 的 mutex_ 是否多余？分层锁设计解析 |

---

> 图例：🐛 Bug ⚡ 性能 💾 内存 🛡️ 设计 🔧 工程
