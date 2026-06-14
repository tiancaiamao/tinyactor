# Task: Phase 4 Task 3b — 完成多 Worker 线程实现

## Context

Task 3 的上半部分已完成（mbox_deliver 修复了 double-enqueue 竞态，match_ok 改为 __thread）。

**但 vm_run 的多线程分支没有实现！** 当前代码 `src/vm.c` 中：

```c
void vm_run(VM *vm) {
    atomic_store(&vm->active_procs, 1);
    if (vm->nworkers <= 1) {
        WorkerCtx wc = { ... };
        worker_loop(&wc);
    } else {
        /* Multi-thread: Task 3 will spawn N workers here.
         * For now, fall back to single-thread. */
        WorkerCtx wc = { ... };     // ← 仍然是单线程！
        worker_loop(&wc);
    }
}
```

这意味着即使 nworkers=8（默认 CPU 核数），实际上还是单线程跑。

## What to Implement

### 1. vm_run — else 分支改为真正的多线程

```c
void vm_run(VM *vm) {
    atomic_store(&vm->active_procs, 1);
    vm->stop = 0;

    if (vm->nworkers <= 1) {
        WorkerCtx wc = { .vm = vm, .current_proc = NULL, .thread_id = 0 };
        worker_loop(&wc);
        return;
    }

    /* Multi-thread mode: spawn N workers */
    vm->workers = malloc(vm->nworkers * sizeof(pthread_t));
    WorkerCtx *wctxs = malloc(vm->nworkers * sizeof(WorkerCtx));

    for (int i = 0; i < vm->nworkers; i++) {
        wctxs[i].vm = vm;
        wctxs[i].current_proc = NULL;
        wctxs[i].thread_id = i;
        pthread_create(&vm->workers[i], NULL, worker_thread_entry, &wctxs[i]);
    }

    for (int i = 0; i < vm->nworkers; i++)
        pthread_join(vm->workers[i], NULL);

    free(wctxs);
}
```

### 2. worker_thread_entry — pthread 入口函数

已有前向声明 `static void *worker_thread_entry(void *arg);`，但没有定义。添加：

```c
static void *worker_thread_entry(void *arg) {
    worker_loop((WorkerCtx *)arg);
    return NULL;
}
```

### 3. worker_loop — I/O poller 互斥 + 多线程退出

当前 worker_loop 的 Phase 2（I/O poll）会同时被多个 worker 调用。需要：

**a) 添加 atomic flag 确保只有一个 worker 在 poll：**
```c
static atomic_int io_polling = 0;  // file-scope
```

**b) Phase 2 改为：**
当 runq 空时：
- 检查 `active_procs == 0` → 设置 `vm->stop = 1`, broadcast `rq_cond`, break
- 尝试 CAS `io_polling` 从 0→1：
  - 成功：执行 I/O poll（现有代码），完后设 `io_polling = 0`
  - 失败：`usleep(1000)` (1ms) 再循环

**c) 循环开始检查 `vm->stop`：**
```c
for (;;) {
    if (vm->stop) break;
    // Phase 1...
    // Phase 2...
}
```

### 4. Stall kill 在多线程下的处理

当 stall > 10000 时：
- 单线程：直接 return（退出唯一的 worker）
- 多线程：设置 `vm->stop = 1`，broadcast condvar，return

```c
if (stall > 10000) {
    // Kill all running procs
    for (...) { q->state = PROC_DEAD; }
    atomic_store(&vm->active_procs, 0);
    vm->stop = 1;
    pthread_cond_broadcast(&vm->rq_cond);
    return;
}
```

### 5. NWORKERS 环境变量

在 `src/main.c` 中，vm_run 之前读取：
```c
char *nw = getenv("NWORKERS");
if (nw) {
    vm->nworkers = atoi(nw);
    if (vm->nworkers < 1) vm->nworkers = 1;
}
```

## Verification

```bash
# 1. Build
make clean && make

# 2. Single-thread regression (default behavior)
pass=0; fail=0; for f in test/scripts/*.lisp; do timeout 10 ./tinyactor "$f" >/dev/null 2>&1; if [ $? -eq 0 ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL: $(basename $f)"; fi; done; echo "PASS: $pass FAIL: $fail"

# 3. Multi-thread concurrent test
NWORKERS=4 timeout 15 ./tinyactor example/scripts/concurrent_test.lisp; echo "EXIT=$?"

# 4. Verify it's actually multi-threaded: check with NWORKERS=2
NWORKERS=2 timeout 15 ./tinyactor example/scripts/concurrent_test.lisp; echo "EXIT=$?"

# 5. 10x stability
for i in $(seq 10); do NWORKERS=4 timeout 10 ./tinyactor example/scripts/concurrent_test.lisp >/dev/null 2>&1 || echo "FAIL run $i"; done

# 6. Verify vm_run actually spawns threads (grep for pthread_create in vm_run)
grep -A 15 'void vm_run' src/vm.c | grep pthread_create
```

## Files to Modify
- `src/vm.c` — vm_run else branch, worker_thread_entry, io_polling flag, worker_loop multi-thread Phase 2
- `src/main.c` — NWORKERS env var

## Rules
1. nworkers=1 must be zero regression
2. GC must not be touched
3. No deadlock: all workers must exit when active_procs==0
4. Build must pass
5. Output DONE: <file list> when complete