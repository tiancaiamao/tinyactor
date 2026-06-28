# 内存优化：支持 1M Actor

## 目标
- 1M idle actors 占用 < 1 GB（~1 KB/actor）
- 当前: 130 KB/actor → 目标: ~0.5-1 KB/actor
- 死进程立即释放内存

## 当前问题分析

| 组件 | 当前大小 | 问题 |
|---|---|---|
| `mem` (heap) | 64 KB | spawn 时预分配，即使空 actor 也占满 |
| `gc_to` (semispace) | 64 KB | spawn 时预分配，从不释放 |
| `gc_roots[256]` | 2 KB | 嵌入结构体，256 个槽大部分时间不用 |
| `catch_stack[8]` | 64 B | 嵌入结构体 |
| `watchers` + `watcher_refs` | 32 B | malloc 4 项，即使无 watcher |
| `pthread_mutex_t` | 40 B | 必须保留 |
| 死进程 | 全部保留 | proc_die 只标记 PROC_DEAD，不释放 mem/gc_to |

**98% 的内存（128KB/130KB）是 mem + gc_to 的预分配。**

## 修改方案

### Task 1: 懒堆分配 (ta.h + src/vm.c)
- `proc_new()`: 不分配 mem 和 gc_to，设为 NULL
- `proc_heap_alloc()`: 首次调用时懒分配 mem (初始 4KB) 和 gc_to (4KB)
- `proc_grow()`: 处理 NULL mem 的初始分配
- GC 安全: gc_collect 检查 mem != NULL
- stack push 也需要触发懒分配

### Task 2: 死进程内存回收 (src/vm.c)
- `proc_die()`: 释放 mem, gc_to, watchers, watcher_refs
- 将释放的 Proc* 加入 free list 或标记可复用
- 保留 Proc 结构体本身（procs[] 中的指针），只释放大块

### Task 3: gc_roots 动态化 (ta.h + src/vm.c + src/gc.c)
- 将 `gc_roots[256]` 改为 `Val *gc_roots`（指针）
- 初始 NULL，首次 push 时分配（如 64 槽）
- 或: 减小到 `gc_roots[32]`（当前 256 槽极少同时使用）

### Task 4: MAX_PROCS 提升 + 1M 测试
- `MAX_PROCS` 从 65536 提升到 1048576 (1M)
- procs[] 数组: calloc(1M, 8) = 8MB virtual（OS demand-paging）
- runq 需要动态增长
- 写 benchmark 脚本: spawn 1M actors 测量 RSS

## 验收标准
1. `make test` 全部通过（174/174）
2. 1M idle actors RSS < 1 GB
3. 死进程内存被正确回收
4. 无内存泄漏（valgrind 或 ASAN）