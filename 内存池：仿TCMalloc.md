# 内存池：仿TCMalloc

## 1. 项目背景
参考 Google 开源的 **TCMalloc (Thread-Caching Malloc)** 核心机制实现，在多线程环境下替代标准的 `malloc/free`。

核心目标是**解决内存碎片问题**并**提高多线程环境下的内存分配效率**。

## 2. 三层架构设计

### 第一层：ThreadCache (线程本地缓存)
*   **定位**：每个线程独享的内存缓存，是分配内存的第一站。
*   **核心特性**：
    *   **无锁访问**：通过 **TLS (Thread Local Storage)** 技术实现，**线程在分配和释放内存时无需加锁**。
    *   **极致效率**：通过自由链表 (FreeList) 管理小块内存，实现 $O(1)$ 时间复杂度的快速分配。
    *   **职能**：满足线程绝大多数的小内存申请需求，消除线程间竞争。

### 第二层：CentralCache (中心缓存)
*   **定位**：全局共享资源池，为ThreadCache 分发内存。
*   **核心特性**：
    *   **桶锁机制 **：不同大小的对象在不同的桶中，**只有当多个线程同时访问同一个桶**时才需加锁（使用自旋锁）。
    *   当 ThreadCache 缓存不足时，批量分配给它；
    *   当 ThreadCache 缓存过多时，回收内存。
    *   **Span 管理**：以 `Span` (由连续页组成的大块内存) 为单位管理内存，并切分成小块供 ThreadCache 使用。

### 第三层：PageCache (页缓存)
*   **定位**：面向操作系统的底层内存管理，负责大块内存的调度。
*   **核心特性**：
    *   **页单位管理**：以“页”为最小单位向系统申请空间。
    *   **内存合并与切分**：负责将大 Span 切分成小 Span 供应 CentralCache，同时在回收时执行**前后地址合并**，缓解外部碎片问题。
    *   **全局锁保护**：涉及全局页映射关系的维护，使用全局锁。

![image-20260718154143521](C:\Users\28783\AppData\Roaming\Typora\typora-user-images\image-20260718154143521.png)

---

## 3. 基础设施层——Common.h

在深入三层架构之前，首先需要理解整个内存池项目的数据基础。`Common.h` 定义了全局常量、数据结构以及大小映射算法，是所有模块的基石。

### 3.1 常量定义与设计考量

```cpp
constexpr size_t ALIGNMENT      = 8;          // 对齐数
constexpr size_t MAX_BYTES      = 256 * 1024; // 256KB，内存池处理的最大内存
constexpr size_t FREE_LIST_SIZE = MAX_BYTES / ALIGNMENT; // = 32768 个槽位
```

**对齐数为什么取 8？**

- **指针存储需求**：在 64 位系统上，指针的大小就是 8 字节。当内存块被释放回收到自由链表时，需要在块内存储指向下一个空闲块的指针，因此每个块的最小粒度必须是 8 字节，这是物理约束。
- **CPU 缓存行对齐**：现代 CPU 的缓存行（Cache Line）通常是 64 字节，8 字节对齐保证了一个缓存行内可以整齐地容纳整数个内存块，避免了跨缓存行访问的性能损失。
- **与 V1 的一致性**：V1 版本同样是 8 字节对齐，保持设计理念的延续。

**FREE_LIST_SIZE 为什么是 32768？**

$$
FREE\_LIST\_SIZE = \frac{MAX\_BYTES}{ALIGNMENT} = \frac{256KB}{8B} = 32768
$$

这意味着自由链表数组有 32768 个槽位，每个槽位对应一种内存块大小（从 8B → 256KB，步进 8B）。这个数组长度看起来很庞大，但实际使用的是 `std::array`，内存是在栈上或静态区分配的，访问是 O(1) 的，这种空间换时间的策略在线程本地缓存（ThreadCache）中尤为高效。

### 3.2 内存块头部——BlockHeader

```cpp
struct BlockHeader
{
    size_t size;        // 内存块大小
    bool   inUse;       // 使用标志
    BlockHeader* next;  // 指向下一个内存块
};
```

**设计意图**：`BlockHeader` 是一个通用的内存块元信息结构体，可以嵌入到任何内存块的头部，用于跟踪和调试。它包含：

| 字段 | 类型 | 作用 |
|:---|:---|:---|
| `size` | `size_t` | 记录该内存块的实际大小（字节数），方便释放时知道该归还到哪个大小类别 |
| `inUse` | `bool` | 使用标志位，标识该块当前是否被用户占用 |
| `next` | `BlockHeader*` | 通用链表指针，用于将内存块串联成单向链表 |

> **注**：在实际实现中，这套三层架构的自由链表通过 `void**` 直接存储指针（详见后续章节），并未使用 `BlockHeader`。该结构体更多是作为预留扩展点——如果未来需要增加内存块校验、日志记录、边界标记等功能，可以直接在此结构体中扩展。

### 3.3 大小类管理器——SizeClass

`SizeClass` 是整个内存池系统的**路由引擎**。它负责将用户请求的任意字节数映射到标准化的内存块大小，并计算出对应的自由链表索引。

#### 3.3.1 向上取整函数 `roundUp`

```cpp
static size_t roundUp(size_t bytes)
{
    return (bytes + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
}
```

**计算原理——位运算对齐法**：

这个一行代码包含了两个经典技巧：

**第一步：`bytes + ALIGNMENT - 1`（向上进位）**
- 假设对齐数是 8，用户请求 10 字节。$10 + 7 = 17$。
- 这步的实质是：如果 `bytes` 不是 8 的倍数，则把低位"溢出"到下一位。

**第二步：`& ~(ALIGNMENT - 1)`（掩码清零低位）**
- `ALIGNMENT - 1 = 7`，二进制为 `...000111`。
- `~7` 取反后为 `...111000`——低 3 位全是 0。
- 按位与操作将低 3 位清零，效果等同于向下取整到 8 的倍数。

**实例演算**：

| 请求大小 | 第一步 `bytes + 7` | 第二步 `& ~7` | 结果 |
|:---|:---|:---|:---|
| 1 | 8 (0b00001000) | 8 & (…11111000) | **8** |
| 8 | 15 (0b00001111) | 15 & (…11111000) | **8** |
| 9 | 16 (0b00010000) | 16 & (…11111000) | **16** |
| 13 | 20 (0b00010100) | 20 & (…11111000) | **16** |

这种方式比使用 `%` 和 `if` 判断快得多，因为它**没有分支跳转**，CPU 流水线不会被打断。

---

#### 3.3.2 索引计算函数 `getIndex`

```cpp
static size_t getIndex(size_t bytes)
{
    bytes = std::max(bytes, ALIGNMENT);                     // 至少为 8
    return (bytes + ALIGNMENT - 1) / ALIGNMENT - 1;         // 映射到数组下标
}
```

**计算逻辑**：

该函数将内存块大小映射到 `freeList_` 数组的下标。设 `ALIGNMENT = 8`：

- 请求 8 字节 → `max(8,8)=8` → `(8+7)/8-1 = 1-1 = 0`（对应 8 字节槽位）
- 请求 9 字节 → `max(9,8)=9` → `(9+7)/8-1 = 2-1 = 1`（对应 16 字节槽位）
- 请求 0 字节 → `max(0,8)=8` → 索引 0（至少分配 8 字节）

**数学本质**：

这个公式等价于：
$$
index = \lceil \frac{bytes}{8} \rceil - 1
$$

本质上就是在做：**"这个请求需要几倍的最小粒度？减 1 就是数组下标"**。

**为什么需要 `std::max`？**

用户可能请求 0 字节（虽然不符合常规，但防御性编程需要处理），直接参与运算会得到 $index = 0 - 1 = -1$，导致数组越界的严重 bug（在 `size_t` 下会变成一个巨大的正数，属于未定义行为）。

**与 V1 的对比**：

V1 版本的索引计算是 `((size + 7) / SLOT_BASE_SIZE) - 1`，核心逻辑完全一致。但 V2 的 `SizeClass` 将其封装为静态工具类，使得 ThreadCache、CentralCache 等多个层级都能复用同一套映射逻辑，避免了代码重复。

---

## 4. 统一接口层——MemoryPool.h

```cpp
class MemoryPool
{
public:
    static void* allocate(size_t size)
    {
        return ThreadCache::getInstance()->allocate(size);
    }

    static void deallocate(void* ptr, size_t size)
    {
        ThreadCache::getInstance()->deallocate(ptr, size);
    }
};
```

### 4.1 外观模式（Facade Pattern）

`MemoryPool` 是面向用户的**唯一入口**，它不包含任何业务逻辑，只做一件事：**将调用转发给 ThreadCache**。

**设计意图**：

| 设计原则 | 体现方式 |
|:---|:---|
| **封装复杂性** | 用户只需 `MemoryPool::allocate(16)`，完全不需要知道 ThreadCache → CentralCache → PageCache 的三层调用链路 |
| **单一入口** | 所有分配请求从同一个接口进入，便于后续添加统一的日志、监控、限流等功能 |
| **与 V1 的接口兼容** | V1 对外提供 `newElement<T>` / `deleteElement<T>`（包含构造/析构），V2 直接退回更底层的 `allocate/deallocate`（原始内存），把对象生命周期管理交给更上层 |

### 4.2 关于对象构造——为什么 V2 不包含 newElement？

观察代码可以发现，V2 的 MemoryPool 只提供原始内存的分配与释放，不像 V1 那样在内存池内部调用 Placement New 和析构函数。这是因为：

1. **职责分离**：TCMalloc 设计理念认为内存分配器应该只负责"哪块内存可以用"这件事，对象的构造属于应用层逻辑。
2. **更灵活**：用户可以自行决定在分配的内存上构造什么类型的对象，甚至可以复用同一块内存构造不同的类型（尽管不常见）。
3. **与标准库对齐**：`malloc/free` 同样不处理构造/析构，这套接口可以直接替换标准库函数。

如果业务层需要对象语义，可以像 V1 那样封装一层薄薄的模板函数：
```cpp
template<typename T, typename... Args>
T* newElement(Args&&... args)
{
    T* p = reinterpret_cast<T*>(MemoryPool::allocate(sizeof(T)));
    if (p) new(p) T(std::forward<Args>(args)...);
    return p;
}

template<typename T>
void deleteElement(T* p)
{
    if (p)
    {
        p->~T();
        MemoryPool::deallocate(p, sizeof(T));
    }
}
```

---

## 5. 第一层：ThreadCache 详细实现

ThreadCache 是整个系统性能的核心所在。它是每个线程私有的本地缓存，承担了**绝大多数的小内存分配请求**。

### 5.1 线程本地存储——TLS（Thread Local Storage）

```cpp
static ThreadCache* getInstance()
{
    static thread_local ThreadCache instance;
    return &instance;
}
```

#### 5.1.1 `thread_local` 关键字详解

`thread_local` 是 C++11 引入的存储类说明符。被它修饰的变量具有以下特性：

| 特性 | 说明 |
|:---|:---|
| **线程独立** | 每个线程拥有该变量的独立副本，互不干扰 |
| **生命周期** | 随线程创建而构造，随线程销毁而析构 |
| **静态行为** | 在函数内部配合 `static` 使用时，每个线程只在首次执行到该行时构造一次，之后就直接返回已有实例 |

**内存布局示意**：

```
线程A的TLS区域:  [ ThreadCache实例A  | freeList_[0..32767] | freeListSize_[0..32767] ]
线程B的TLS区域:  [ ThreadCache实例B  | freeList_[0..32767] | freeListSize_[0..32767] ]
线程C的TLS区域:  [ ThreadCache实例C  | freeList_[0..32767] | freeListSize_[0..32767] ]
```

每个线程的 `freeList_` 是完全隔离的，线程 A 修改自己的自由链表时，线程 B 完全感知不到——**这就是 ThreadCache 不需要加锁的根本原因**。

#### 5.1.2 为什么这是无锁设计的基础？

在多线程编程中，锁的代价主要来自两方面：

1. **上下文切换**：如果锁被占用，操作系统需要挂起当前线程，执行上下文切换（保存寄存器 → 切换页表 → 加载新线程），这个过程通常在微秒级。
2. **缓存失效**：当锁在不同 CPU 核心间传递时，每个核心的 L1/L2 缓存行需要同步，这在 NUMA 架构下尤其昂贵。

`thread_local` 让每个线程拥有私有的数据副本，**从物理上消除了竞争条件**，不需要锁来保护共享状态。这是比 CAS（Compare-And-Swap）无锁算法更彻底的无锁方案——不是"无锁但竞争"，而是"根本不需要竞争"。

### 5.2 ThreadCache 的内部数据结构

```cpp
std::array<void*, FREE_LIST_SIZE>  freeList_;     // 自由链表数组，每个元素是链表头指针
std::array<size_t, FREE_LIST_SIZE> freeListSize_;  // 每个自由链表的当前长度计数
```

**设计解读**：

- **`freeList_`**：32768 个元素的 `void*` 数组。`freeList_[i]` 指向第 `i` 个大小类别的自由链表头。如果为空（`nullptr`），说明该大小的内存块已耗尽。
- **`freeListSize_`**：与 `freeList_` 一一对应，记录每条自由链表中有多少个可用的内存块。这个计数器并非功能必需品（遍历链表也能知道长度），但它让 `deallocate` 中的回收判断变成了 O(1) 操作——直接比较数字而非遍历链表。

### 5.3 `allocate` 函数——分配流程

```cpp
void* ThreadCache::allocate(size_t size)
{
    if (size == 0) size = ALIGNMENT;    // 防御：0字节请求按8字节处理
    if (size > MAX_BYTES)               // 大对象分支：走系统malloc
        return malloc(size);

    size_t index = SizeClass::getIndex(size);

    freeListSize_[index]--;             // 预减计数

    // 快速路径：自由链表有货
    if (void* ptr = freeList_[index])
    {
        freeList_[index] = *reinterpret_cast<void**>(ptr);
        return ptr;
    }

    // 慢速路径：本地缓存不够，向CentralCache批量申请
    return fetchFromCentralCache(index);
}
```

**流程图**：

```
allocate(size)
    │
    ├── size == 0? ──→ size = 8（最小分配单位）
    │
    ├── size > 256KB? ──→ malloc(size) → 返回（大对象直接走系统调用）
    │
    └── 计算 index = getIndex(size)
         │
         freeListSize_[index]--
         │
         ├── freeList_[index] != nullptr? ──YES──→ 从链表头部取一块 → 返回
         │
         └── NO ──→ fetchFromCentralCache(index) → 返回
```

#### 5.3.1 快速路径中的指针操作

```cpp
if (void* ptr = freeList_[index])
{
    freeList_[index] = *reinterpret_cast<void**>(ptr);
    return ptr;
}
```

这是 ThreadCache 中**最常被执行**的三行代码。99% 的分配请求走这条路径。要彻底理解它，需要从"自由链表在内存中到底长什么样"这个问题入手。

---

**第一步：理解自由链表的物理存储方式**

自由链表不是一个独立的数据结构（不像 `std::list` 那样在堆上分配节点），而是**利用了空闲内存块自己来存储链表信息**。

假设用户请求 24 字节内存，经过 `SizeClass::getIndex(24)` 映射后，实际分配 32 字节。此时 `freeList_[index]` 对应的自由链表中，链接着若干个 32 字节的空闲内存块。

下面用**具体的假想内存地址**来演示。假设当前自由链表中有 3 个空闲块：

```
物理内存地址      该地址处存储的 8 字节内容（解释为 void*）    含义
─────────────────────────────────────────────────────────────
0x1000   ───→   0x2000                                   "下一块在 0x2000"
0x1008 ~ 0x101F  任意垃圾数据（空闲状态下无意义）

0x2000   ───→   0x3000                                   "下一块在 0x3000"
0x2008 ~ 0x201F  任意垃圾数据

0x3000   ───→   nullptr                                  "我是最后一块"
0x3008 ~ 0x301F  任意垃圾数据
```

每个 32 字节的空闲块，**只有前 8 个字节有意义**——里面存的是链表中下一个空闲块的地址。8 字节正好是一个 `void*` 指针的大小（在 64 位系统下）。

```
一块 32 字节的空闲内存:
┌──────────────────────┬──────────────────────────────────────┐
│   前 8 字节           │      剩余 24 字节                     │
│   存储 next 指针      │      任意内容（无人关心）              │
│   (void* 大小)        │                                      │
└──────────────────────┴──────────────────────────────────────┘
```

此时 `freeList_[index]` 作为链表头指针，存储的是 **第一个空闲块的地址**：

```
freeList_[index] = 0x1000
```

用图表示整个链表：

```
freeList_[index]                   
     │                            
     ▼                            
┌─────────┐     ┌─────────┐     ┌─────────┐
│ 0x1000  │     │ 0x2000  │     │ 0x3000  │
│         │     │         │     │         │
│ 前8字节: │────→│ 前8字节: │────→│ 前8字节: │────→ nullptr
│ 0x2000  │     │ 0x3000  │     │ nullptr │
│         │     │         │     │         │
│ 后24字节 │     │ 后24字节 │     │ 后24字节 │
│ (垃圾)  │     │ (垃圾)  │     │ (垃圾)  │
└─────────┘     └─────────┘     └─────────┘
```

---

**第二步：逐行拆解代码**

现在用这个假想状态来跟踪代码的每一步。初始状态：

```
freeList_[index] = 0x1000

内存内容:
  地址 0x1000 处的前8字节:  存储的值是 0x2000（指向下一块的指针）
  地址 0x2000 处的前8字节:  存储的值是 0x3000
  地址 0x3000 处的前8字节:  存储的值是 nullptr
```

**第 1 行：`if (void* ptr = freeList_[index])`**

```cpp
if (void* ptr = freeList_[index])  // ptr = 0x1000，非空，进入 if 体
```

这行做了一件事：把 `freeList_[index]` 的值（`0x1000`）赋给局部变量 `ptr`。如果链表为空（`freeList_[index] == nullptr`），则不进入 if，走下面的 `fetchFromCentralCache` 慢路径。

执行后：
```
ptr = 0x1000   （即将返回给用户的内存地址）
```

---

**第 2 行：`freeList_[index] = *reinterpret_cast<void**>(ptr);`**

这一行是整个 ThreadCache 中最"难读"的代码。把它拆成三小步理解：

> **小步 ①：`ptr` 的值是 `0x1000`**
>
> 它现在是 `void*` 类型，含义是"内存地址 0x1000"。

> **小步 ②：`reinterpret_cast<void**>(ptr)`**
>
> 这是类型转换。它**不改变任何内存数据**，只改变了编译器对这个变量"类型身份"的看法：
>
> - 转换前：`ptr` 是 `void*`——"一个指向某块内存的指针"
> - 转换后：`reinterpret_cast<void**>(ptr)` 是 `void**`——"一个指向指针的指针"，即"指向内存中某个 `void*` 的指针"
>
> **物理本质**：`0x1000` 这个地址还是那个地址，只是编译器现在认为"从这个地址开始的 8 字节，里面存的是一个 `void*` 指针（即下一块的地址）"。

> **小步 ③：最外层的 `*`（解引用）**
>
> `*reinterpret_cast<void**>(ptr)` 的意思是：**把 0x1000 开始的 8 个字节读出来，解释为一个 `void*` 指针**。
>
> 根据前面假想的初始状态，`0x1000` 处存储的是 `0x2000`。所以：
>
> ```
> *reinterpret_cast<void**>(0x1000) → 读出 8 字节 → 0x2000
> ```
>
> 这就取到了链表中的**下一个空闲块的地址**。

然后把读出的值 `0x2000` 赋给 `freeList_[index]`：

```cpp
freeList_[index] = 0x2000;  // 链表头从 0x1000 移到 0x2000
```

---

**第 3 行：`return ptr;`**

```cpp
return ptr;  // 返回 0x1000 给用户
```

把 `0x1000`（刚才从链表头取下的那块 32 字节内存）返回给用户使用。

---

**第三步：执行后的状态**

```
操作前:                                    操作后:
                                            
freeList_[index] = 0x1000                  freeList_[index] = 0x2000
     │                                          │
     ▼                                          ▼
┌─────────┐    ┌─────────┐    ┌─────────┐    ┌─────────┐    ┌─────────┐
│ 0x1000  │    │ 0x2000  │    │ 0x3000  │    │ 0x1000  │    │ 0x2000  │    │ 0x3000  │
│ next ───│───→│ next ───│───→│ next ───│→0  │ (返回)  │    │ next ───│───→│ next ───│→0
│         │    │         │    │         │    │ 用户使用 │    │         │    │         │
└─────────┘    └─────────┘    └─────────┘    └─────────┘    └─────────┘    └─────────┘
     ▲                                            ▲
     └── ptr 指向这里，即将返回给用户               └── 新的链表头

返回给用户: 0x1000
链表剩余:   0x2000 → 0x3000 → nullptr
```

---

**第四步：用一个"完整申请序列"来串联理解**

假设用户连续调用三次 `allocate(24)`（每次映射到 32 字节大小类），以下是每次调用的完整过程：

**初始状态**：
```
freeList_[index] = 0x1000
链表: 0x1000 → 0x2000 → 0x3000 → nullptr
```

**第一次 `allocate(24)`**：
| 步骤 | 代码 | 发生了什么 |
|:---|:---|:---|
| 读取链表头 | `ptr = freeList_[index]` | `ptr = 0x1000` |
| 读取 next 指针 | `*reinterpret_cast<void**>(ptr)` | 从 `0x1000` 处读出 `0x2000` |
| 更新链表头 | `freeList_[index] = ...` | `freeList_[index] = 0x2000` |
| 返回 | `return ptr` | 用户拿到 `0x1000` |

```
之后: freeList_[index] = 0x2000
      链表: 0x2000 → 0x3000 → nullptr
```

**第二次 `allocate(24)`**：
| 步骤 | 代码 | 发生了什么 |
|:---|:---|:---|
| 读取链表头 | `ptr = freeList_[index]` | `ptr = 0x2000` |
| 读取 next 指针 | `*reinterpret_cast<void**>(ptr)` | 从 `0x2000` 处读出 `0x3000` |
| 更新链表头 | `freeList_[index] = ...` | `freeList_[index] = 0x3000` |
| 返回 | `return ptr` | 用户拿到 `0x2000` |

```
之后: freeList_[index] = 0x3000
      链表: 0x3000 → nullptr
```

**第三次 `allocate(24)`**：
| 步骤 | 代码 | 发生了什么 |
|:---|:---|:---|
| 读取链表头 | `ptr = freeList_[index]` | `ptr = 0x3000` |
| 读取 next 指针 | `*reinterpret_cast<void**>(ptr)` | 从 `0x3000` 处读出 `nullptr` |
| 更新链表头 | `freeList_[index] = ...` | `freeList_[index] = nullptr` |
| 返回 | `return ptr` | 用户拿到 `0x3000` |

```
之后: freeList_[index] = nullptr
      链表为空
```

**第四次 `allocate(24)`**：
```
if (void* ptr = freeList_[index])  // freeList_[index] == nullptr
                                   // 条件为 false，不进入 if
                                   // 走慢路径 fetchFromCentralCache()
```

---

**第五步：与 deallocate 的逆操作对照**

理解了 `allocate` 怎么从链表取走一块，再来看 `deallocate` 怎么把一块还回去，就会发现它们是对称的：

```cpp
// deallocate 中的归还操作（头插法）
*reinterpret_cast<void**>(ptr) = freeList_[index];  // 让归还块的 next 指向当前链表头
freeList_[index] = ptr;                              // 更新链表头为归还块
```

用具体地址演示。假设归还地址 `0x1000`，当前链表只有一个 `0x5000`：

```
归还前:
  freeList_[index] = 0x5000
  链表: 0x5000 → nullptr

归还操作:
  *reinterpret_cast<void**>(0x1000) = 0x5000   // 0x1000 的前 8 字节写入 0x5000
  freeList_[index] = 0x1000                     // 链表头移到 0x1000

归还后:
  freeList_[index] = 0x1000
  链表: 0x1000 → 0x5000 → nullptr
```

**allocate 和 deallocate 的对比总结**：

| 操作 | 代码模式 | 效果 |
|:---|:---|:---|
| **取走一块** | `ptr = head; head = *reinterpret_cast<void**>(ptr)` | 弹出链表头 |
| **归还一块** | `*reinterpret_cast<void**>(ptr) = head; head = ptr` | 压入链表头 |

两者组合在一起，就是一个**以空闲内存块自身为节点的无额外开销的单向链表栈**。

---

**第六步：为什么初学者会觉得困惑？**

这段代码让人困惑的原因通常有三个：

1. **双重指针 `void**`**的概念跳跃**：平时写代码很少用到"指向指针的指针"。但在这里，空闲块的前 8 字节**确实存储了一个指针**，所以需要一个"指向这个被存储的指针"的类型来读写它。

2. **同一个地址，两种身份**：地址 `0x1000` 对用户来说是"存数据的地方"，对内存池来说是"存 next 指针的地方"。`reinterpret_cast` 就是在切换这两种视角——它不改变任何内存内容，只改变"如何解读这片内存"。

3. **没有显式的链表节点结构体**：传统链表教学会用 `struct Node { Data d; Node* next; }`，但这里没有任何结构体——空闲块本身就是节点，`next` 直接写进块的前 8 字节。这省掉了所有节点的堆分配开销。

**核心要记住的一句话**：
> 空闲内存块的前 8 字节就是它的 `next` 指针。`*reinterpret_cast<void**>(ptr)` 不过是"把 ptr 指向地址的前 8 字节读出来"的一种语法写法。等价于：从内存地址 ptr 处，按 `void*` 的尺寸读取一个值。

#### 5.3.2 `fetchFromCentralCache`——向中央缓存批量申请

```cpp
void* ThreadCache::fetchFromCentralCache(size_t index)
{
    void* start = CentralCache::getInstance().fetchRange(index);
    if (!start) return nullptr;

    // 取链表头部一块返回给用户
    void* result = start;
    freeList_[index] = *reinterpret_cast<void**>(start);

    // 计算这一批有多少块，更新计数
    size_t batchNum = 0;
    void* current = start;
    while (current != nullptr)
    {
        batchNum++;
        current = *reinterpret_cast<void**>(current);
    }
    freeListSize_[index] += batchNum;

    return result;
}
```

**批量获取策略**：当 ThreadCache 的某个大小类别耗尽时，它一次性从 CentralCache 获取一批（而非一个）内存块。这样做的好处是：

- **摊销跨层开销**：越过 ThreadCache → CentralCache 的边界是有成本的（自旋锁、可能的 PageCache 调用），一次取一批可以将这个成本分摊到多个分配请求上。
- **减少竞争频率**：如果每次只取一个，ThreadCache 会频繁访问 CentralCache 的自旋锁，增加线程间的竞争概率。

### 5.4 `deallocate` 函数——释放流程

```cpp
void ThreadCache::deallocate(void* ptr, size_t size)
{
    if (size > MAX_BYTES)
    {
        free(ptr);
        return;
    }

    size_t index = SizeClass::getIndex(size);

    // 头插法归还到自由链表
    *reinterpret_cast<void**>(ptr) = freeList_[index];
    freeList_[index] = ptr;
    freeListSize_[index]++;

    // 检查是否需要归还一部分给CentralCache
    if (shouldReturnToCentralCache(index))
        returnToCentralCache(freeList_[index], size);
}
```

#### 5.4.1 回收决策——什么时候归还给 CentralCache？

```cpp
bool ThreadCache::shouldReturnToCentralCache(size_t index)
{
    size_t threshold = 256; // 每个大小类别最多缓存256块
    return (freeListSize_[index] > threshold);
}
```

这个阈值是 TCMalloc 设计的核心调优参数之一：

| 参数选择 | 效果 |
|:---|:---|
| 阈值太小（如 8） | ThreadCache 频繁归还，增加 CentralCache 的竞争 |
| 阈值太大（如 100000） | 某线程囤积大量空闲内存，其他线程却因 CentralCache 无货而需要从 PageCache 分配 |
| **256** | 经验值，在"快速重用"和"防止囤积"之间取得平衡 |

#### 5.4.2 `returnToCentralCache`——归还策略

```cpp
void ThreadCache::returnToCentralCache(void* start, size_t size)
{
    size_t index = SizeClass::getIndex(size);
    size_t alignedSize = SizeClass::roundUp(size);
    size_t batchNum = freeListSize_[index];
    if (batchNum <= 1) return;

    // 保留 1/4，归还 3/4
    size_t keepNum = std::max(batchNum / 4, size_t(1));
    size_t returnNum = batchNum - keepNum;

    // 遍历到第 keepNum 个节点，断开链表
    char* current = static_cast<char*>(start);
    char* splitNode = current;
    for (size_t i = 0; i < keepNum - 1; ++i)
    {
        splitNode = reinterpret_cast<char*>(*reinterpret_cast<void**>(splitNode));
        if (splitNode == nullptr) break;
    }

    if (splitNode != nullptr)
    {
        void* nextNode = *reinterpret_cast<void**>(splitNode);
        *reinterpret_cast<void**>(splitNode) = nullptr; // 断开

        freeList_[index] = start;       // 保留部分仍挂在 ThreadCache
        freeListSize_[index] = keepNum;

        if (returnNum > 0 && nextNode != nullptr)
            CentralCache::getInstance().returnRange(nextNode, returnNum * alignedSize, index);
    }
}
```

**保留 1/4 的设计考量**：全部归还和全部保留都不合理：

- **全部归还**：ThreadCache 刚还完，用户马上又申请，又要去 CentralCache 取——"抖动"（thrashing）。
- **全部保留**：线程可能不再使用该大小类别，造成内存浪费。
- **保留 1/4**：折中方案，既保留了本地缓存的热度，又将多余内存还给全局池子供其他线程使用。

---

## 6. 第二层：CentralCache 详细实现

CentralCache 是全局共享的资源调度层，承担了 ThreadCache 和 PageCache 之间的桥梁角色。

### 6.1 自旋锁——桶锁机制

```cpp
std::array<std::atomic_flag, FREE_LIST_SIZE> locks_;
```

#### 6.1.1 什么是自旋锁？

自旋锁（Spin Lock）是一种**忙等待**的锁实现。与 `std::mutex` 不同，当自旋锁被占用时，线程不会进入内核态挂起，而是在用户态循环检查锁状态。

```cpp
// CentralCache 中的加锁方式
while (locks_[index].test_and_set(std::memory_order_acquire))
{
    std::this_thread::yield(); // 让出 CPU 时间片，避免纯空转
}
```

#### 6.1.2 `std::atomic_flag` 与 `test_and_set`

`std::atomic_flag` 是 C++ 中最简单的原子类型——它只有一个布尔值，但它保证是**无锁**（lock-free）的。

`test_and_set(memory_order)` 是一个原子操作：
1. 读取当前的标志位值（true/false）
2. 将标志位设置为 true
3. 返回读取到的旧值

整个过程在硬件层面用一条 CAS 指令完成，不可被中断。如果返回 `false`，说明之前没人持有锁，**你获取到了锁**。如果返回 `true`，说明别人还在用，继续循环等待。

#### 6.1.3 为什么用自旋锁而不用互斥锁？

| 对比维度 | `std::mutex`（互斥锁） | 自旋锁 |
|:---|:---|:---|
| 等待方式 | 线程挂起，进入内核调度队列 | 用户态循环，不进入内核 |
| 上下文切换 | 有（保存/恢复寄存器、切换页表） | 无 |
| 适用场景 | 临界区执行时间长（毫秒级） | 临界区执行时间极短（微秒级） |
| 开销 | 挂起/唤醒各约几微秒 | 每次 CAS 约几十纳秒 |

CentralCache 的临界区操作非常短——本质上就是从链表头取一个指针或挂一个指针，几个 CPU 指令就完成了。这种情况下互斥锁的挂起/唤醒开销远大于临界区本身，自旋锁才是正确的选择。

**`std::this_thread::yield()` 的作用**：避免纯粹的 `while(flag.test_and_set())` 空转浪费 CPU 资源。`yield()` 告诉调度器"我可以等一等，先让其他线程运行"，这在超线程架构下尤其重要——如果物理核心的两个逻辑线程都在空转等锁，谁都做不了有用功。

#### 6.1.4 桶锁——精细化的锁粒度

这是 TCMalloc 对比传统 `malloc` 的又一个性能利器。传统方案只有一个全局堆锁——任何线程做任何大小的分配都要竞争同一把锁。CentralCache 有 32768 个独立的锁，每个锁只保护一个大小类别的链表：

```
线程A 请求 8字节  → 竞争 locks_[0]
线程B 请求 16字节 → 竞争 locks_[1]
// 两个线程不互相阻塞！它们操作的是不同的锁
```

只有在多线程同时请求**相同大小**的内存时才会发生锁竞争，这在统计概率上大大降低了竞争的概率。

### 6.2 Span 追踪器——SpanTracker

```cpp
struct SpanTracker {
    std::atomic<void*>  spanAddr{nullptr};   // Span 的起始地址
    std::atomic<size_t> numPages{0};         // Span 包含的页数
    std::atomic<size_t> blockCount{0};       // Span 被切分成的总块数
    std::atomic<size_t> freeCount{0};        // 当前空闲的块数
};
```

**作用**：当 CentralCache 从 PageCache 获取一个 Span（连续多页内存），并把它切分成小块分给 ThreadCache 后，需要跟踪每个块属于哪个 Span。当某个 Span 的**所有小块都归还到 CentralCache** 时（`freeCount == blockCount`），这个 Span 就可以整体归还给 PageCache，由 PageCache 执行合并或直接释放给操作系统。

**为什么不用 `std::map` 而用固定数组？**

```cpp
std::array<SpanTracker, 1024> spanTrackers_;
std::atomic<size_t> spanCount_{0};  // 当前已使用的 Span 追踪器数量
```

- **`std::map`**：每次查找需要 O(log n) 的红黑树遍历，而且动态分配节点本身也依赖 `malloc`——在内存分配器内部再用 `malloc` 会造成递归依赖的问题。
- **固定数组**：O(n) 的线性遍历，但 n 最多 1024。对于内存管理这种毫秒级敏感的路径，固定数组的缓存局部性更好——连续的内存布局意味着 CPU 预取（prefetch）更有效。

### 6.3 `fetchRange`——向 ThreadCache 分发内存

```cpp
void* CentralCache::fetchRange(size_t index)
{
    // 1. 获取自旋锁
    while (locks_[index].test_and_set(std::memory_order_acquire))
        std::this_thread::yield();

    void* result = centralFreeList_[index].load(std::memory_order_relaxed);

    if (!result)
    {
        // 2. 中心缓存为空 → 从 PageCache 申请新 Span
        size_t size = (index + 1) * ALIGNMENT;
        result = fetchFromPageCache(size);

        // 3. 将原始内存切分成小块，串成链表
        char* start = static_cast<char*>(result);
        size_t numPages = (size <= SPAN_PAGES * PageCache::PAGE_SIZE) ?
                          SPAN_PAGES : (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;
        size_t blockNum = (numPages * PageCache::PAGE_SIZE) / size;

        // 构建链表：相邻块用头部的8字节连接
        for (size_t i = 1; i < blockNum; ++i)
        {
            void* current = start + (i - 1) * size;
            void* next    = start + i * size;
            *reinterpret_cast<void**>(current) = next;
        }
        *reinterpret_cast<void**>(start + (blockNum - 1) * size) = nullptr;

        // 提取第一块返回，其余挂到 centralFreeList_
        void* next = *reinterpret_cast<void**>(result);
        *reinterpret_cast<void**>(result) = nullptr;
        centralFreeList_[index].store(next, std::memory_order_release);

        // 记录 Span 追踪信息
        size_t trackerIndex = spanCount_++;
        spanTrackers_[trackerIndex].spanAddr.store(start, ...);
        spanTrackers_[trackerIndex].numPages.store(numPages, ...);
        spanTrackers_[trackerIndex].blockCount.store(blockNum, ...);
        spanTrackers_[trackerIndex].freeCount.store(blockNum - 1, ...);
    }
    else
    {
        // 4. 中心缓存有货 → 直接从链表头取一块
        void* next = *reinterpret_cast<void**>(result);
        *reinterpret_cast<void**>(result) = nullptr;
        centralFreeList_[index].store(next, std::memory_order_release);

        // 更新对应 Span 的空闲计数
        SpanTracker* tracker = getSpanTracker(result);
        if (tracker)
            tracker->freeCount.fetch_sub(1, std::memory_order_release);
    }

    locks_[index].clear(std::memory_order_release);
    return result;
}
```

#### 6.3.1 `fetchFromPageCache`——向底层申请 Span

```cpp
void* CentralCache::fetchFromPageCache(size_t size)
{
    size_t numPages = (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

    if (size <= SPAN_PAGES * PageCache::PAGE_SIZE)  // size <= 32KB
        return PageCache::getInstance().allocateSpan(SPAN_PAGES);  // 固定申请8页
    else
        return PageCache::getInstance().allocateSpan(numPages);    // 按需申请
}
```

**固定 8 页策略的设计考量**：

对于大部分小内存请求（≤32KB），CentralCache 统一向 PageCache 申请 8 页（32KB）。这样的好处是：

1. **减少 PageCache 的碎片化**：如果每次都按精确需求申请，PageCache 会很快被各种大小的 Span 碎片填满。
2. **批量管理效率**：一次 8 页的 Span 可以切分成大量小块，够 ThreadCache 用很久，减少了 PageCache 的调用频率。

### 6.4 `returnRange`——回收 ThreadCache 归还的内存

```cpp
void CentralCache::returnRange(void* start, size_t size, size_t index)
{
    size_t blockSize = (index + 1) * ALIGNMENT;
    size_t blockCount = size / blockSize;

    // 加锁
    while (locks_[index].test_and_set(std::memory_order_acquire))
        std::this_thread::yield();

    // 1. 找到归还链表的尾节点
    void* end = start;
    size_t count = 1;
    while (*reinterpret_cast<void**>(end) != nullptr && count < blockCount)
    {
        end = *reinterpret_cast<void**>(end);
        count++;
    }

    // 2. 头插法：将 CentralCache 的现有链表接到归还链表后面
    void* current = centralFreeList_[index].load(std::memory_order_relaxed);
    *reinterpret_cast<void**>(end) = current;
    centralFreeList_[index].store(start, std::memory_order_release);

    // 3. 延迟归还检查
    size_t currentCount = delayCounts_[index].fetch_add(1, ...) + 1;
    auto currentTime = std::chrono::steady_clock::now();

    if (shouldPerformDelayedReturn(index, currentCount, currentTime))
        performDelayedReturn(index);

    locks_[index].clear(std::memory_order_release);
}
```

### 6.5 延迟归还机制——防止"抖动"

```cpp
static const size_t MAX_DELAY_COUNT = 48;
static const std::chrono::milliseconds DELAY_INTERVAL{1000}; // 1000ms
```

**为什么需要延迟归还？**

想象这个场景：线程 A 刚归还了一批 16 字节内存块给 CentralCache，CentralCache 立刻把这个 Span 还给 PageCache。紧接着线程 B 又申请 16 字节，PageCache 又得重新切出一个 Span……这种"刚还又要"的抖动会严重影响性能。

**双重条件触发**：

```cpp
bool CentralCache::shouldPerformDelayedReturn(size_t index, size_t currentCount,
    std::chrono::steady_clock::time_point currentTime)
{
    if (currentCount >= MAX_DELAY_COUNT) return true;       // 条件1：累积了足够多次归还
    auto lastTime = lastReturnTimes_[index];
    return (currentTime - lastTime) >= DELAY_INTERVAL;       // 条件2：距上次归还超过1秒
}
```

这个设计类似于操作系统的**脏页写回**策略——不急于立即归还，而是等待"够了"再统一处理，减少反复操作。

### 6.6 `performDelayedReturn`——批量归还 Span

```cpp
void CentralCache::performDelayedReturn(size_t index)
{
    delayCounts_[index].store(0, std::memory_order_relaxed);
    lastReturnTimes_[index] = std::chrono::steady_clock::now();

    // 统计 centralFreeList_ 中每个 Span 的空闲块数
    std::unordered_map<SpanTracker*, size_t> spanFreeCounts;
    void* currentBlock = centralFreeList_[index].load(...);

    while (currentBlock)
    {
        SpanTracker* tracker = getSpanTracker(currentBlock);
        if (tracker) spanFreeCounts[tracker]++;
        currentBlock = *reinterpret_cast<void**>(currentBlock);
    }

    // 更新空闲计数，全部空闲则归还 PageCache
    for (const auto& [tracker, newFreeBlocks] : spanFreeCounts)
        updateSpanFreeCount(tracker, newFreeBlocks, index);
}
```

### 6.7 `updateSpanFreeCount`——归还整个 Span 的判定

```cpp
void CentralCache::updateSpanFreeCount(SpanTracker* tracker, size_t newFreeBlocks, size_t index)
{
    size_t oldFreeCount = tracker->freeCount.load(std::memory_order_relaxed);
    size_t newFreeCount = oldFreeCount + newFreeBlocks;
    tracker->freeCount.store(newFreeCount, std::memory_order_release);

    // 关键判断：该 Span 的所有块都空闲了吗？
    if (newFreeCount == tracker->blockCount.load(std::memory_order_relaxed))
    {
        void* spanAddr = tracker->spanAddr.load(...);
        size_t numPages = tracker->numPages.load(...);

        // 从 centralFreeList_ 中摘除属于该 Span 的所有块
        // ...（遍历链表，按地址范围过滤，重组链表）

        // 归还完整的 Span 给 PageCache
        PageCache::getInstance().deallocateSpan(spanAddr, numPages);
    }
}
```

**判断逻辑的精妙之处**：只有当 `freeCount == blockCount`——即该 Span 被切分出的**每一个**小块都回到了 CentralCache 的空闲链表——才说明这个 Span 完全没人用了，可以安全归还。如果还有一个块在外面（被 ThreadCache 持有或被用户使用），就不能动。

---

## 7. 第三层：PageCache 详细实现

PageCache 是整个系统的底层，直接与操作系统交互，管理以页（Page）为单位的连续内存块——Span。

### 7.1 PageCache 的内部数据结构

```cpp
static const size_t PAGE_SIZE = 4096; // 4K页

struct Span
{
    void*  pageAddr; // 页起始地址
    size_t numPages; // 页数
    Span*  next;     // 链表指针
};

std::map<size_t, Span*> freeSpans_;   // 按页数分类的空闲Span链表
std::map<void*, Span*> spanMap_;      // 页地址 → Span 的映射（回收时快速查找）
std::mutex mutex_;                    // 全局互斥锁
```

#### 7.1.1 为什么 PageCache 使用 `std::mutex` 而不是自旋锁？

与 CentralCache 的"微秒级取指针"不同，PageCache 的操作可能涉及：

- **系统调用**：`mmap` 向操作系统申请内存，这本身就是毫秒级的阻塞操作。
- **链表遍历与 Span 合并**：释放时需要查找相邻 Span 并执行合并，可能存在多次指针操作。

这些操作的耗时远超自旋锁的合理使用范围——如果让几十个线程同时在 PageCache 上自旋等待一个 `mmap` 返回，CPU 资源会被严重浪费。因此这里使用 `std::mutex`，让等待的线程挂起，把 CPU 让给有实际工作可做的线程。

#### 7.1.2 `freeSpans_`——按页数分级管理

`std::map<size_t, Span*>` 存储了当前可用的空闲 Span，按页数排序：

```
freeSpans_:
  1页  → Span(0x1000) → Span(0x5000) → nullptr
  3页  → Span(0x2000) → nullptr
  8页  → Span(0x8000) → nullptr
  16页 → Span(0xA000) → nullptr
```

同一页数的 Span 组成单向链表，不同页数互不干扰。

#### 7.1.3 `spanMap_`——O(log n) 的地址反查

当回收一块内存时，PageCache 只知道起始地址，需要快速找到对应的 `Span` 对象来获取页数等信息。`spanMap_` 将物理地址映射到 `Span*`，使用 `std::map` 可以在 O(log n) 时间内完成查找。

### 7.2 `allocateSpan`——分配 Span（含切分逻辑）

```cpp
void* PageCache::allocateSpan(size_t numPages)
{
    std::lock_guard<std::mutex> lock(mutex_);  // 全局锁

    // 1. 查找 ≥numPages 的最小空闲 Span
    auto it = freeSpans_.lower_bound(numPages);
    if (it != freeSpans_.end())
    {
        Span* span = it->second;

        // 从链表中移除
        if (span->next)
            freeSpans_[it->first] = span->next;
        else
            freeSpans_.erase(it);

        // 2. 如果 Span 比需要的更大，切分
        if (span->numPages > numPages)
        {
            Span* newSpan = new Span;
            newSpan->pageAddr = static_cast<char*>(span->pageAddr)
                                + numPages * PAGE_SIZE;
            newSpan->numPages = span->numPages - numPages;
            newSpan->next = nullptr;

            // 剩余部分放回空闲列表
            auto& list = freeSpans_[newSpan->numPages];
            newSpan->next = list;
            list = newSpan;

            span->numPages = numPages;
        }

        spanMap_[span->pageAddr] = span;
        return span->pageAddr;
    }

    // 3. 没有合适的空闲 Span，向系统申请
    void* memory = systemAlloc(numPages);
    Span* span = new Span;
    span->pageAddr = memory;
    span->numPages = numPages;
    spanMap_[memory] = span;
    return memory;
}
```

#### 7.2.1 `lower_bound`——最佳匹配策略

```cpp
auto it = freeSpans_.lower_bound(numPages);
```

`std::map::lower_bound(key)` 返回第一个 **≥ key** 的元素的迭代器。例如请求 5 页：

- 如果 `freeSpans_` 中有 5 页的条目：直接命中
- 如果只有 3 页和 8 页：返回 8 页的条目

这是**最佳匹配**（Best-Fit）策略的一种高效实现——从所有 ≥ 需求的空闲块中选取最小的那个，减少浪费。

#### 7.2.2 Span 切分——"大块拆小块"

```
切分前:
  8页 Span: [page0][page1][page2][page3][page4][page5][page6][page7]

请求 3 页:
  切分后:
    返回给调用者: [page0][page1][page2]  (3页)
    放回freeSpans_: [page3][page4][page5][page6][page7]  (5页)
```

这种切分策略确保了内存的高效利用——不会因为请求 3 页就给 8 页导致浪费 5 页。

### 7.3 `deallocateSpan`——释放 Span（含合并逻辑）

```cpp
void PageCache::deallocateSpan(void* ptr, size_t numPages)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = spanMap_.find(ptr);
    if (it == spanMap_.end()) return;  // 不是我们分配的内存

    Span* span = it->second;

    // === 尝试与后面的 Span 合并 ===
    void* nextAddr = static_cast<char*>(ptr) + numPages * PAGE_SIZE;
    auto nextIt = spanMap_.find(nextAddr);

    if (nextIt != spanMap_.end())
    {
        Span* nextSpan = nextIt->second;

        // 检查 nextSpan 是否在空闲链表中
        bool found = false;
        auto& nextList = freeSpans_[nextSpan->numPages];

        if (nextList == nextSpan) {
            nextList = nextSpan->next;
            found = true;
        } else if (nextList) {
            Span* prev = nextList;
            while (prev->next) {
                if (prev->next == nextSpan) {
                    prev->next = nextSpan->next;
                    found = true;
                    break;
                }
                prev = prev->next;
            }
        }

        // 从空闲列表中找到才合并（防止使用中的块被合并）
        if (found)
        {
            span->numPages += nextSpan->numPages;
            spanMap_.erase(nextAddr);
            delete nextSpan;
        }
    }

    // 通过头插法将合并后的 Span 插入空闲列表
    auto& list = freeSpans_[span->numPages];
    span->next = list;
    list = span;
}
```

#### 7.3.1 地址合并——对抗外部碎片

这是 PageCache 解决**外部碎片**的核心机制。当一个 Span 被释放时，PageCache 检查它的相邻地址（向后）是否也有空闲 Span：

```
释放前:
  Span A (空闲, 3页, 0x1000-0x3FFF)
  Span B (刚释放, 2页, 0x4000-0x5FFF)

释放后:
  合并Span: (空闲, 5页, 0x1000-0x5FFF)
```

这个策略被称为 **Buddy System 的后向合并**。真正的 TCMalloc 还支持前向合并（检查前一页是否空闲），但由于这里只维护了 `spanMap_`（以起始地址为 key），前向合并需要额外的查找，当前实现简化了这一逻辑。

#### 7.3.2 合并的安全条件

合并前必须确认相邻 Span **确实在空闲链表中**——即它当前没有被任何 ThreadCache 或 CentralCache 使用。代码遍历 `freeSpans_` 链表来验证这一点：只有真正空闲的 Span 才参与合并。

### 7.4 `systemAlloc`——与操作系统的接口

```cpp
void* PageCache::systemAlloc(size_t numPages)
{
    size_t size = numPages * PAGE_SIZE;

    // 使用 mmap 从操作系统申请匿名内存
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) return nullptr;

    memset(ptr, 0, size);  // 清零
    return ptr;
}
```

#### 7.4.1 为什么使用 `mmap` 而不是 `sbrk` 或 `malloc`？

| 接口 | 特点 | 适合场景 |
|:---|:---|:---|
| `sbrk` | 移动堆顶指针，只能增长不能收缩 | 已过时，不推荐使用 |
| `malloc` (`brk`) | 小内存用 brk，大内存用 mmap，内部有复杂管理 | **不能在内存池项目内部使用**——会造成循环依赖 |
| `mmap` | 直接向内核申请匿名页，可随时 `munmap` 归还 | 内存池底层的最佳选择 |

**关键问题：为什么不能用 `malloc` 实现内存池？**

如果 `systemAlloc` 内部调用 `malloc` 来从系统获取内存，那么——当用户代码将 `malloc` 替换为 `MemoryPool::allocate` 后，`systemAlloc` → `malloc` → `MemoryPool::allocate` → `systemAlloc` 就会形成**无限递归**。使用 `mmap` 彻底绕开了这个问题，直接从操作系统内核分配物理页。

#### 7.4.2 `mmap` 参数详解

```cpp
mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
```

| 参数 | 值 | 含义 |
|:---|:---|:---|
| `addr` | `nullptr` | 让内核选择起始地址 |
| `length` | `numPages * 4096` | 请求的内存大小 |
| `prot` | `PROT_READ \| PROT_WRITE` | 可读可写 |
| `flags` | `MAP_PRIVATE \| MAP_ANONYMOUS` | 私有映射 + 匿名（不关联文件） |
| `fd` | `-1` | MAP_ANONYMOUS 下被忽略 |
| `offset` | `0` | 无偏移 |

`memset(ptr, 0, size)` 确保分配的内存是干净的。因为 `mmap` 返回的页面在使用前被内核标记为写时复制（Copy-on-Write）的零页，显式清零强制内核分配物理页，避免了后续使用时的缺页中断（Page Fault）开销。

---

## 8. 完整流程串联——一次内存分配的生命周期

### 8.1 分配路径（Allocation Flow）

```
用户调用 MemoryPool::allocate(24)
    │
    ▼
[ThreadCache::allocate]
    │  index = getIndex(24) → (24+7)/8-1 = 2（对应 24→32 字节大小类）
    │
    ├── freeList_[2] 有货？
    │   ├── YES → 取链表头，返回 (99%的情况走这条)
    │   └── NO  ─────────────────────────────────────────┐
    │                                                     ▼
    │                              [ThreadCache::fetchFromCentralCache(2)]
    │                                  │
    │                                  ▼
    │                              [CentralCache::fetchRange(2)]
    │                                  │  获取自旋锁 locks_[2]
    │                                  │
    │                                  ├── centralFreeList_[2] 有货？
    │                                  │   ├── YES → 取一块返回
    │                                  │   └── NO  ───────────────────────┐
    │                                  │                                    ▼
    │                                  │              [CentralCache::fetchFromPageCache]
    │                                  │                  │  计算页数: 32B ≤ 32KB → 固定8页
    │                                  │                  ▼
    │                                  │              [PageCache::allocateSpan(8)]
    │                                  │                  │  获取全局 mutex_
    │                                  │                  │  lower_bound(8) 查找空闲 Span
    │                                  │                  │
    │                                  │                  ├── 找到 → 取出、切分（如需要）、返回
    │                                  │                  └── 没找到 → systemAlloc → mmap 系统调用
    │                                  │
    │                                  │  拿到 8 页 (32KB) 原始内存
    │                                  │  切分成 1024 个 32B 小块
    │                                  │  串成链表，留一块返回，其余挂在 centralFreeList_[2]
    │                                  │  记录 SpanTracker
    │                                  │  释放自旋锁
    │                                  │
    │                                  ▼
    │                              返回一批 32B 块给 ThreadCache
    │                              ThreadCache 取一块返回用户，其余存入 freeList_[2]
    │
    ▼
用户拿到 void* 指针 (指向32字节对齐的内存块)
```

### 8.2 释放路径（Deallocation Flow）

```
用户调用 MemoryPool::deallocate(ptr, 24)
    │
    ▼
[ThreadCache::deallocate]
    │  index = 2, freeListSize_[2]++
    │  头插法挂入 freeList_[2]
    │
    ├── freeListSize_[2] > 256？
    │   ├── NO → 完成 (99%的情况)
    │   └── YES ────────────────────────────────────────┐
    │                                                     ▼
    │                              [ThreadCache::returnToCentralCache]
    │                                  保留 1/4 (64块)，归还 3/4 (192块)
    │                                  │
    │                                  ▼
    │                              [CentralCache::returnRange]
    │                                  获取自旋锁 locks_[2]
    │                                  头插法挂入 centralFreeList_[2]
    │                                  delayCounts_[2]++
    │                                  │
    │                                  ├── 满足延迟归还条件？
    │                                  │   ├── NO → 释放锁，完成
    │                                  │   └── YES ────────────────────────────┐
    │                                  │                                         ▼
    │                                  │              [CentralCache::performDelayedReturn]
    │                                  │                  遍历 centralFreeList_[2]，按 Span 分组统计
    │                                  │                  │
    │                                  │                  └── 某 Span 的 freeCount == blockCount？
    │                                  │                       ├── YES → 从链表中摘除该 Span 所有块
    │                                  │                       │         调用 PageCache::deallocateSpan
    │                                  │                       │         │
    │                                  │                       │         ▼
    │                                  │                       │    [PageCache::deallocateSpan]
    │                                  │                       │        获取全局 mutex_
    │                                  │                       │        检查相邻 Span，合并
    │                                  │                       │        插入 freeSpans_ 空闲列表
    │                                  │                       │        (未立即 munmap，缓存待重用)
    │                                  │                       │
    │                                  │                       └── NO → 继续等待
    │                                  │
    │                                  释放自旋锁 locks_[2]
    │
    ▼
完成
```

### 8.3 各级缓存的命中率分析

| 层级 | 典型命中率 | 操作开销 | 锁开销 |
|:---|:---|:---|:---|
| **ThreadCache** | ~95-99% | O(1)，几个 CPU 指令 | **无锁**（TLS隔离） |
| **CentralCache** | ~1-5% | O(1)，链表操作 + 自旋锁 | 桶级自旋锁（竞争概率低） |
| **PageCache** | <0.1% | O(log n) 查找 + mmap | 全局互斥锁 |

绝大多数的分配请求在 ThreadCache 就完成了，**不涉及任何锁，不涉及跨线程通信**，这正是 TCMalloc 在多线程环境下比标准 `malloc` 快数倍的根本原因。

---

## 9. 涉及相关知识

### 9.1 内存序（Memory Order）与无锁编程

在多核 CPU 系统中，每个核心有自己的缓存（L1/L2 cache），对内存的写入不会立即被其他核心看到。编译器优化和 CPU 乱序执行也会改变指令的实际执行顺序。**内存序（Memory Order）**就是用来控制这种可见性和顺序的工具。

#### 9.1.1 六种内存序概述

C++11 定义了六种内存序，按约束从弱到强排列：

| 内存序 | 约束 | 典型场景 |
|:---|:---|:---|
| `memory_order_relaxed` | 仅保证原子性，不保证顺序 | 计数器、统计信息 |
| `memory_order_consume` | 数据依赖（很少使用，建议用 acquire） | — |
| `memory_order_acquire` | 读操作：后续读写不能重排到此之前 | 锁的获取、消费者读 |
| `memory_order_release` | 写操作：之前读写不能重排到此之后 | 锁的释放、生产者写 |
| `memory_order_acq_rel` | acquire + release 的合并 | Read-Modify-Write 操作 |
| `memory_order_seq_cst` | 全局顺序一致性（默认，最重） | 需要严格顺序时 |

#### 9.1.2 项目中的实际使用

**场景 1：非关键状态的松散加载**

```cpp
void* result = centralFreeList_[index].load(std::memory_order_relaxed);
```

这里只是读取一个指针，后续还有 CAS 或锁的保护做同步。用 `relaxed` 告诉 CPU：你可以用缓存中的旧值，我不在意时刻的精确性，错了后面会重试。这样 CPU 不会主动刷新缓存行，性能最高。

**场景 2：自旋锁的获取与释放**

```cpp
// 获取锁
while (locks_[index].test_and_set(std::memory_order_acquire))
    std::this_thread::yield();

// 释放锁
locks_[index].clear(std::memory_order_release);
```

这是一个经典的 `acquire-release` 配对。`acquire` 确保获取锁之后的读操作能看到上一位持锁者（release）之前的所有写入。`release` 确保释放锁之前的所有写入对下一位获取者（acquire）可见。

**为什么不用 `seq_cst`（默认）？**

`seq_cst` 要求所有 CPU 核心对所有原子操作的执行顺序达成全局一致——这需要跨核心的总线通信（如 x86 的 `MFENCE`），代价很高。在内存分配器的热路径上，`acquire-release` 已经提供了必要的同步保证，用 `seq_cst` 只是徒增开销。

#### 9.1.3 fetch_add / fetch_sub

```cpp
tracker->freeCount.fetch_sub(1, std::memory_order_release);
delayCounts_[index].fetch_add(1, std::memory_order_relaxed);
```

- `fetch_add(n)`：原子地将值增加 n，返回**旧值**
- `fetch_sub(n)`：原子地将值减少 n，返回**旧值**

与 `load + store` 的手动方式不同，`fetch_add/fetch_sub` 是一个不可分割的原子指令（在 x86 上是 `LOCK XADD`），即使多个线程同时调用也不会出现丢失更新的问题。

### 9.2 自由链表的嵌入式指针技术

```cpp
// 取下一块
freeList_[index] = *reinterpret_cast<void**>(ptr);

// 挂入链表
*reinterpret_cast<void**>(ptr) = freeList_[index];
freeList_[index] = ptr;
```

#### 9.2.1 核心思想

传统链表的节点定义是：

```cpp
struct ListNode {
    Data data;        // 数据区
    ListNode* next;   // 指针区
};
```

但内存池分配出去的块是给用户存数据的。用户还回来时，这块内存不再被用户使用——**它的数据区可以被内存池"征用"来存储管理信息**。

```
内存块处于"已分配"状态时:
  [ 用户数据区 (size字节)  ]

内存块处于"空闲"状态时:
  [ next指针 (8字节) ][ 无用区域 (size-8字节) ]
```

这就是**嵌入式指针（Embedded Pointer）**技术——利用空闲块自己的空间存储链表指针，零额外内存开销。

#### 9.2.2 `reinterpret_cast<void**>` 的含义

`void**` 是指向 `void*` 的指针，大小为 8 字节（在 64 位系统下）。

```cpp
*reinterpret_cast<void**>(ptr) = freeList_[index];
```

这行代码做了三件事：

1. `reinterpret_cast<void**>(ptr)`：告诉编译器"把 `ptr` 这个地址当作 `void**` 来解读"
2. `*` 解引用：读取/写入该地址处的 8 个字节
3. `= freeList_[index]`：将旧的链表头指针写入这 8 个字节

**物理本质**：没有创建任何新对象，没有复制任何管理结构，只是直接操作了内存块前 8 字节的内容。这就是为什么最小分配粒度必须是 8 字节——如果一个请求只分配了 4 字节，那这 4 字节不够存一个 `void*`，归还时就无法嵌入指针。

### 9.3 单例模式——线程安全的懒汉实现

项目中的所有缓存层都使用了经典的**线程安全单例**：

```cpp
static ThreadCache* getInstance()
{
    static thread_local ThreadCache instance;
    return &instance;
}

static CentralCache& getInstance()
{
    static CentralCache instance;
    return instance;
}
```

#### 9.3.1 C++11 的静态局部变量保证

在 C++11 之前，多线程环境下的懒汉单例需要**双重检查锁定（DCLP）**这种容易出错的技巧。C++11 标准明确规定：

> 如果多个线程同时进入一个函数，该函数内有一个 `static` 局部变量，并且该变量的初始化尚未完成，**只有一个线程会执行初始化，其他线程会阻塞直到初始化完成**。

这意味着编译器会自动插入必要的同步代码（通常在初始化前后各加一个内存屏障），程序员不需要手动加锁。这个特性被称为"Magic Statics"。

#### 9.3.2 ThreadCache 的 `thread_local` + `static` 组合

```cpp
static thread_local ThreadCache instance;
```

这是一个特殊的组合：
- `static`：保证每个线程内只构造一次
- `thread_local`：保证每个线程有独立的实例

线程 A 第一次调用 `getInstance()` 时，在线程 A 的 TLS 区域构造 `instance`。线程 B 同理。两个线程的 `instance` 是不同的对象，地址不同，互不影响。

### 9.4 `std::array` vs `std::vector` vs C 数组

项目大量使用了 `std::array`：

```cpp
std::array<void*, FREE_LIST_SIZE>  freeList_;      // 32768个元素
std::array<SpanTracker, 1024>      spanTrackers_;   // 1024个元素
std::array<std::atomic_flag, FREE_LIST_SIZE> locks_; // 32768个自旋锁
```

| 容器 | 栈/堆分配 | 大小 | 何时用 |
|:---|:---|:---|:---|
| `T arr[N]` | 栈（局部）/ 静态区（全局） | 编译期固定 | 最简单但功能最少 |
| `std::array<T,N>` | 同 C 数组 | 编译期固定 | 需要迭代器、size()、at() 等标准接口 |
| `std::vector<T>` | 堆（动态分配） | 运行时可变 | 大小不确定、需要动态增长 |

对于内存池这种**编译期就知道大小**的场景，`std::array` 是最佳选择：它避免了 `std::vector` 的堆分配开销（这本身就是内存池要解决的问题），同时提供了丰富的 STL 接口。

**代价**：`32768 * sizeof(void*) = 256KB`（每个 ThreadCache 的自由链表数组），加上 `freeListSize_` 的 `32768 * sizeof(size_t) = 256KB`，每个线程的 ThreadCache 实例约 512KB。如果系统有 100 个线程，这就是约 50MB 的开销。但相比于 pthread 默认 8MB 的栈空间来说，这个开销是可以接受的，而且这些内存分布在 TLS 中，不会造成中心化的内存压力。

### 9.5 `std::chrono` 时间库

```cpp
std::chrono::steady_clock::time_point lastReturnTimes_[FREE_LIST_SIZE];
static const std::chrono::milliseconds DELAY_INTERVAL{1000};
```

**时钟类型选择**：

| 时钟类型 | 特点 | 适合场景 |
|:---|:---|:---|
| `system_clock` | 系统时间，可被管理员/NTP 修改 | 需要显示给用户的时间 |
| `steady_clock` | 单调递增，**永远不会倒退** | 性能计时、间隔测量 |
| `high_resolution_clock` | 最高精度（通常就是 steady_clock） | 微基准测试 |

延迟归还机制选择了 `steady_clock`，因为它只关心"距上次过去了多久"，不需要知道"现在几点"。如果系统时间被 NTP 校时回拨，`system_clock` 会得到负的时间差，导致逻辑错误。

### 9.6 C++17 结构化绑定

```cpp
for (const auto& [tracker, newFreeBlocks] : spanFreeCounts)
{
    updateSpanFreeCount(tracker, newFreeBlocks, index);
}
```

这是 C++17 的结构化绑定（Structured Binding），它将 `std::pair` 或 `std::tuple` 的元素直接解包到命名变量中。等价于传统写法：

```cpp
for (const auto& entry : spanFreeCounts)
{
    SpanTracker* tracker = entry.first;
    size_t newFreeBlocks = entry.second;
    // ...
}
```

CMakeLists.txt 中指定了 `CXX_STANDARD 17`，因此这条特性可以使用。

---

## 10. 与 V1 的架构对比

| 维度 | 内存池 V1 | 内存池 V2（仿 TCMalloc） |
|:---|:---|:---|
| **架构层级** | 单层（多个 MemoryPool + 哈希桶路由） | **三层**（ThreadCache → CentralCache → PageCache） |
| **线程安全** | 全局 `std::atomic<Slot*>` CAS 无锁 | **TLS 隔离**（首层无锁）+ 桶级自旋锁 + 全局互斥锁 |
| **扩容单位** | Block（4096B），每个 MemoryPool 独立申请 | **Span（多页）**，PageCache 统一管理，支持切分与合并 |
| **内存回收** | 归还到本池 freeList，无法跨池 | 三层逐级回收，**支持整 Span 归还 PageCache→合并→mmap** |
| **最大内存** | 512B（超过走 malloc） | **256KB**（步进 8B，覆盖更广） |
| **对系统接口** | `operator new` | **`mmap`**（避免循环依赖） |
| **碎片控制** | 固定 Block 大小，无合并 | **地址合并**，缓解外部碎片 |
| **适用场景** | 单线程或低并发 | **高并发多线程**（每线程本地缓存） |

### 10.1 V2 相比 V1 的核心改进

1. **ThreadCache 的 TLS 设计**：V1 的所有线程共享一套 `MemoryPool[64]`，即使使用 CAS 无锁操作，高并发下 CAS 的重试次数也会激增。V2 让每个线程拥有自己的 `freeList_[32768]`，命中率最高可达 99%，彻底消除了热路径上的竞争。

2. **动态 Span 管理**：V1 的 Block 是固定的 4096B，对于某些大小类别会造成较大浪费。V2 的 PageCache 按需切分 Span——可以分配 1 页、3 页、8 页，更灵活。

3. **内存归还机制**：V1 只从系统拿内存，从不归还（直到进程退出）。V2 的 SpanTracker + 延迟归还机制，让完全空闲的 Span 可以被回收并合并——长期运行的服务器程序不会无限堆高内存占用。

4. **覆盖范围扩大**：V1 只管理 512B 以内的小对象，V2 扩展到 256KB，能覆盖更广泛的使用场景。

---

## 11. 总结——TCMalloc 设计哲学

这套三层架构体现了 TCMalloc 的几个核心设计原则：

1. **让常见路径尽可能快**：95%+ 的分配请求在 ThreadCache 的 `if (void* ptr = freeList_[index])` 这一行就完成了——O(1)、无锁、无系统调用。

2. **按热度分层**：越热的路径越靠近线程（ThreadCache），越冷的路径越靠近系统（PageCache）。锁的开销、系统调用的开销只在冷路径上发生。

3. **批量操作摊销开销**：ThreadCache 批量从 CentralCache 获取，CentralCache 批量从 PageCache 获取。单次跨层操作的成本被分摊到多次分配上。

4. **惰性归还与缓存**：不急于归还空闲内存——ThreadCache 保留 1/4 防止抖动，CentralCache 延迟归还防止频繁 Span 回收，PageCache 不立即 munmap 而是缓存 Span。每一层都有自己的"缓存哲学"。

5. **物理隔离优于逻辑隔离**：TLS 在物理上隔离线程数据，比"共享 + 加锁"或"共享 + CAS"都更彻底。最好的并发控制不是"有效地竞争"，而是"根本不需要竞争"。
