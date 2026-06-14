# Task: Phase 4 Task 3 — 多 Worker 线程

## Context

Task 1 添加了线程基础设施（runq mutex, tls_current_proc, atomic pid/active_procs）。Task 2 改造了 mailbox 为 Heap Fragment。当前 `vm_run` 在 `nworkers > 1` 时仍然只跑单线程 fallback。

本任务启用真正的多 worker 线程：vm_run 启动 N 个 pthread，每个 worker 竞争 runq 取 actor 执行。

## Current State

`vm_run` (vm.c ~line 369):
```c
void vm_run(VM *vm) {
    atomic_store(&vm->active_procs, 1);
    if (vm->nworkers <= 1) {
        WorkerCtx wc = { ... };
        worker_loop(&wc);
    } else {
        // FALLBACK: still single-thread!
        WorkerCtx wc = { ... };
        worker_loop(&wc);
    }
}
```

`worker_loop` (vm.c ~line 384): Phase 1 runs ready actors, Phase 2 polls WAIT_IO when queue empty. Exit when nfds==0.

## What to Implement

### 1. vm_run — 多线程启动

当 `nworkers > 1` 时：

```c
void vm_run(VM *vm) {
    atomic_store(&vm->active_procs, 1);

    if (vm->nworkers <= 1) {
        WorkerCtx wc = { .vm = vm, .current_proc = NULL, .thread_id = 0 };
        worker_loop(&wc);
        return;
    }

    // Multi-worker mode
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

需要 `worker_thread_entry` 作为 pthread 的入口函数（调用 worker_loop）。

### 2. worker_loop — 多线程安全改造

**I/O poller 问题**：当前每个 worker 都会执行 Phase 2（poll WAIT_IO）。多线程下多个 worker 同时 poll 同样的 fd 是浪费的，也可能导致重复唤醒。

**解决方案**：用 atomic flag 确保只有一个 worker 在 poll：

```c
static atomic_int io_polling = 0;  // 0 = idle, 1 = someone is polling

// In worker_loop, after Phase 1:
if (!ran) {
    // runq is empty
    if (atomic_load(&vm->active_procs) == 0) break;  // all dead → exit

    // Try to become the I/O poller
    int expected = 0;
    if (atomic_compare_exchange_strong(&io_polling, &expected, 1)) {
        // I'm the poller — collect WAIT_IO fds, poll, wake ready
        // ... existing Phase 2 code ...
        atomic_store(&io_polling, 0);
    } else {
        // Another worker is polling — short sleep then retry
        usleep(1000);  // 1ms
    }
}
```

### 3. Exit Condition

所有 worker 退出的条件：
- `atomic_load(&vm->active_procs) == 0` — 所有 actor 已死
- 此时所有 worker 应 break 退出 worker_loop

但有一个微妙问题：当一个 worker 决定退出时，其他 worker 可能正在 condvar_wait 或 usleep。需要确保它们也被唤醒。

方案：当一个 worker 退出时，设置 `vm->stop = 1` 并 broadcast condvar：
```c
if (atomic_load(&vm->active_procs) == 0) {
    vm->stop = 1;
    pthread_cond_broadcast(&vm->rq_cond);
    break;
}
```

每个 worker 在循环开始检查 `vm->stop`。

### 4. nworkers 配置

- `vm->nworkers` 默认 = `sysconf(_SC_NPROCESSORS_ONLN)`（已在 Task 1 的 vm_new 中设置）
- 支持环境变量 `NWORKERS` 覆盖（在 main.c 中读取）：
```c
char *nw = getenv("NWORKERS");
if (nw) vm->nworkers = atoi(nw);
if (vm->nworkers < 1) vm->nworkers = 1;
```

### 5. Stall Counter 调整

当前 stall counter 在多线程下有问题：多个 worker 可能同时递增 stall。但 stall 的检查是 per-worker 的（每次 worker_loop 调用有自己的 stall 变量）。

实际上 stall 的含义是"这个 worker 连续 10000 次没有状态变化" → 杀死所有 RUNNING 进程。多线程下每个 worker 独立计数，这是合理的（每个 worker 独立判断死锁）。

但"杀死所有进程"的操作需要同步：一个 worker 杀了所有进程，其他 worker 不应重复操作。用 vm->stop 标志保护即可。

## Files to Modify

- `src/vm.c` — vm_run 多线程启动, worker_loop I/O poll 互斥, worker_thread_entry
- `src/main.c` — NWORKERS 环境变量支持
- `ta.h` — 可能需要 worker_thread_entry 声明（如果前向声明需要）

## Important Constraints

1. **nworkers=1 零回归**：单线程模式行为不变
2. **GC 零改动**
3. **Skynet 不变量**：一个 actor 同一时刻只在一个 worker 上执行
4. **I/O poll 互斥**：同一时刻只有一个 worker 在 poll
5. **无死锁**：所有 actor 死亡后所有 worker 必须退出

## Verification

```bash
# 1. Build
make clean && make

# 2. 单线程回归 (nworkers=1)
NWORKERS=1 pass=0; fail=0; for f in test/scripts/*.lisp; do NWORKERS=1 timeout 10 ./tinyactor "$f" >/dev/null 2>&1; if [ $? -eq 0 ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL: $(basename $f)"; fi; done; echo "PASS: $pass FAIL: $fail"

# 3. 多线程 concurrent test (关键！)
NWORKERS=4 timeout 15 ./tinyactor example/scripts/concurrent_test.lisp
echo "EXIT=$?"

# 4. 多线程 echo server
NWORKERS=4 timeout 5 ./tinyactor example/scripts/echo_server.lisp &
sleep 2; echo "multi-thread" | nc -w 2 localhost 8090; echo "EXIT=$?"
pkill -f tinyactor

# 5. 连续 10 次无 crash
for i in $(seq 10); do NWORKERS=4 timeout 10 ./tinyactor example/scripts/concurrent_test.lisp 2>/dev/null || echo "FAIL run $i"; done

# 6. 单线程兼容
timeout 15 ./tinyactor example/scripts/concurrent_test.lisp
```

## Rules
1. GC 不能修改
2. nworkers=1 必须零回归
3. 无死锁、无竞态 crash
4. Build must pass
5. Output DONE: <file list> when complete