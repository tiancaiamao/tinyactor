# Task: Phase 4 Task 1 — 基础线程设施

## Context

TinyActor 是一个用 C99 实现的嵌入式 Actor 并发脚本语言 VM。当前是单线程模型：一个 `vm_run()` 循环，一个 run queue（裸数组无锁），一个 `vm->current_proc` 全局指针。

Phase 4 将其升级为 Skynet 风格的多线程调度器：N 个 worker 线程共享一个全局 runq，一个 actor 同一时刻只在一个线程上执行。

**本任务（Task 1）只做基础线程设施**，默认 `nworkers=1`（单线程退化模式），不改变现有行为。多 worker 启用是 Task 3 的事。

## What to Implement

### 1. ta.h — 结构体改造

**新增头文件引用：**
```c
#include <pthread.h>
#include <stdatomic.h>
```

**新增常量：**
```c
#define MAX_PROCS 65536
```

**新增 MsgFragment 结构体（定义但不接线，Task 2 使用）：**
```c
typedef struct MsgFragment {
    struct MsgFragment *next;
    int   size;
    Val   root;
    uint8_t data[];
} MsgFragment;
```

**新增 WorkerCtx 结构体：**
```c
typedef struct {
    VM   *vm;
    Proc *current_proc;
    int   thread_id;
} WorkerCtx;
```

**Proc 改动：**
- 新增 `pthread_mutex_t mbox_lock;` 字段（Task 1 中初始化和销毁，Task 2 中使用）

**VM 改动：**
- 删除 `Proc *current_proc;`（改为 WorkerCtx.current_proc，per-thread）
- `int next_pid` → `atomic_int next_pid`
- 新增 `atomic_int active_procs;`
- 新增 `pthread_mutex_t rq_lock;`
- 新增 `pthread_cond_t rq_cond;`
- `int next_pid` 改为 atomic（上面已说）
- 新增 `int nworkers;`（默认值 = CPU 核数）
- 新增 `volatile int stop;`
- 新增 `pthread_t *workers;`（worker 线程数组）
- `runq` 相关字段（rq_head, rq_tail, rq_count）中，`rq_count` 改为 `atomic_int`
- `procs_cap` 在 vm_new 中设为 `MAX_PROCS`，`procs` 预分配 `calloc(MAX_PROCS, sizeof(Proc*))`

### 2. vm.c — 核心改造

**runq 加锁：**

`runq_enqueue(vm, pid)` 加 mutex 保护 + condvar signal：
```c
void runq_enqueue(VM *vm, int pid) {
    pthread_mutex_lock(&vm->rq_lock);
    // ... 环形 buffer 写入 ...
    atomic_fetch_add(&vm->rq_count, 1);
    pthread_cond_signal(&vm->rq_cond);
    pthread_mutex_unlock(&vm->rq_lock);
}
```

新增 `runq_trydequeue(vm)` — 非阻塞出队：
```c
int runq_trydequeue(VM *vm) {
    if (atomic_load(&vm->rq_count) == 0) return -1;
    pthread_mutex_lock(&vm->rq_lock);
    if (atomic_load(&vm->rq_count) == 0) {
        pthread_mutex_unlock(&vm->rq_lock);
        return -1;
    }
    int pid = vm->runq[vm->rq_head % vm->rq_cap];
    vm->rq_head++;
    atomic_fetch_sub(&vm->rq_count, 1);
    pthread_mutex_unlock(&vm->rq_lock);
    return pid;
}
```

**current_proc → per-thread：**

当前代码中所有 `vm->current_proc` 的引用改为通过 WorkerCtx 访问。

关键修改点：
- `vm_step()` 的签名需要接受 WorkerCtx 或通过其他方式获取 current_proc
- C 函数调用（OP_CCALL）中 net_read 等使用 `vm->current_proc` 的地方
- 最简方案：用 `static __thread Proc *tls_current_proc;`，在 worker_loop 入口设置

**proc_new 改动：**
- `p->pid = vm->next_pid++` → `p->pid = atomic_fetch_add(&vm->next_pid, 1)`
- `pthread_mutex_init(&p->mbox_lock, NULL)` 初始化 mailbox 锁
- procs[] 不再 realloc（预分配 MAX_PROCS）

**proc_die 改动：**
- 新增 `atomic_fetch_sub(&vm->active_procs, 1)`
- proc_die 中的 monitor 消息发送暂不改（仍在目标堆上分配，nworkers=1 下安全）

**vm_run 改造：**

当前 vm_run 是一个 for(;;) 循环，包含 Phase 1（run ready actors）和 Phase 2（poll WAIT_IO actors）。

改造为：
```c
void vm_run(VM *vm) {
    // 初始化 active_procs = 1（main 进程）
    atomic_store(&vm->active_procs, 1);
    
    if (vm->nworkers <= 1) {
        // 单线程退化：直接在当前线程跑 worker_loop
        WorkerCtx wc = { .vm = vm, .current_proc = NULL, .thread_id = 0 };
        worker_loop(&wc);
    } else {
        // 多线程：Task 3 实现
        // Task 1 中先 fallback 到单线程
        WorkerCtx wc = { .vm = vm, .current_proc = NULL, .thread_id = 0 };
        worker_loop(&wc);
    }
}
```

**worker_loop 函数：**

将当前 vm_run 的 for(;;) 循环逻辑提取到 `worker_loop(WorkerCtx *wc)` 中：
- Phase 1：用 runq_trydequeue 取 actor，执行 MAX_REDUCTIONS 步
- Phase 2：当 runq 为空时，poll WAIT_IO actors，唤醒就绪的
- 退出条件：runq 为空 + 没有 WAIT_IO actors + active_procs == 0
- 用 `wc->current_proc` 替代 `vm->current_proc`
- 设置 `tls_current_proc = p` 在执行每个 actor 之前

### 3. 所有 `vm->current_proc` 引用更新

搜索所有使用 `vm->current_proc` 的地方：
- `src/vm.c` — vm_step 中 OP_CCALL 等地方
- `src/net.c` — net_read 中 `Proc *p = vm->current_proc`
- `src/val.c` — 检查是否有引用
- 其他文件

统一改为 `tls_current_proc`（__thread 变量）。

### 4. Makefile

链接选项加 `-lpthread`：
```makefile
LDFLAGS = -lpthread
```

### 5. main.c

如果 main.c 中设置了 `vm->nworkers` 或调用了 vm_run，确保兼容。

## Files to Modify

- `ta.h` — 结构体定义（Proc, VM, 新增 MsgFragment, WorkerCtx）
- `src/vm.c` — runq 加锁, worker_loop, vm_run 改造, proc_new/die 改动, current_proc → tls
- `src/net.c` — `vm->current_proc` → `tls_current_proc`
- `src/api.c` — vm_new 中初始化 threading 字段, vm_free 中清理
- `src/val.c` — 检查 current_proc 引用
- `Makefile` — 加 -lpthread

## Important Design Constraints

1. **GC 零改动**：`gc_collect()`, `gc_copy_val()`, semispace 逻辑一行都不改
2. **Skynet 不变量**：一个 actor 同一时刻只在一个线程上执行
3. **nworkers=1 兼容**：单线程模式下行为与 Phase 3 完全一致
4. **mailbox 暂不改**：Task 1 中 mailbox 仍是 Val 数组，send 仍用 val_deep_copy。mbox_lock 加上但不用（Task 2 改用）
5. **MsgFragment 只定义不使用**：Task 2 会接线
6. **C99 + pthreads**：不用 C11 threads.h

## Verification

```bash
# 1. 编译通过
make clean && make

# 2. 全部测试通过（46个，bytes-basic pre-existing fail）
pass=0; fail=0; for f in test/scripts/*.lisp; do timeout 10 ./tinyactor "$f" >/dev/null 2>&1; if [ $? -eq 0 ]; then pass=$((pass+1)); else fail=$((fail+1)); fi; done; echo "PASS: $pass  FAIL: $fail"

# 3. Echo server 正常
cd example && make echo_server && ./echo_server &
sleep 1; echo "hello" | nc -w 2 localhost 8090; pkill -f echo_server

# 4. HTTP server 正常
cd example && make http_server && ./http_server &
sleep 1; curl -s http://localhost:8080/; pkill -f http_server

# 5. Concurrent test 正常
timeout 15 ./tinyactor example/scripts/concurrent_test.lisp

# 6. example binaries 也能编译
cd example && make clean && make
```

## Rules
1. READ BEFORE WRITE — grep 确认 API 存在再使用
2. BUILD MUST PASS — 实现后必须构建成功
3. GC 不能动 — gc_collect 和 gc_copy_val 不修改
4. nworkers=1 零回归 — 所有现有测试必须通过
5. Output DONE: <file list> when complete