# memory_pool

仿 TCMalloc 的三层内存池

## 构建

```bash
mkdir -p build && cd build
cmake ..
make
```

## 使用

```cpp
#include "MemoryPool.h"
using namespace wevix_memoryPool;

// 分配
void* p = MemoryPool::allocate(128);

// 释放（size 必须与分配时一致）
MemoryPool::deallocate(p, 128);
```

## 运行测试

```bash
./bin/unit_test
./bin/perf_test
```
