# Task: Phase 4 Task 4 — I/O Poller 独立线程

## Context

Task 3 实现了多 worker 线程。I/O polling 当前嵌入在 worker_loop 中（用 atomic CAS 确保只有一个 worker poll）。这有两个问题：
1. 被选为 poller 的 worker 在 poll(100ms) 期间不处理 actor
2. 复杂度高（CAS + io_polling flag + usleep 分支）

本任务将 I/O poller 提取为**独立线程**，worker 不再做 I/O poll。

## Current State

`worker_loop` 的多线程 Phase 2（vm.c ~line 502-555）：
```c
// CAS to become poller
if (atomic_compare_exchange_strong(&io_polling, &expected, 1)) {
    // collect WAIT_IO fds, poll(100ms), wake ready
    atomic_store(&io_polling, 0);
} else {
    usleep(1000);
}
```

## What to Implement

### 1. io_poller_loop — 独立 I/O poller 线程

```c
static void *io_poller_thread(void *arg) {
    VM *vm = (VM *)arg;
    while (!vm->stop) {
        struct pollfd pfds[1024];
        int pids[1024];
        int nfds = 0;

        for (int i = 0; i < vm->procs_cap && nfds < 1024; i++) {
            Proc *p = vm->procs[i];
            if (p && p->state == PROC_WAIT_IO) {
                pfds[nfds].fd = p->wait_fd;
                pfds[nfds].events = p->wait_events;
                pfds[nfds].revents = 0;
                pids[nfds] = i;
                nfds++;
            }
        }

        if (nfds > 0) {
            poll(pfds, (nfds_t)nfds, 100);
            for (int i = 0; i < nfds; i++) {
                if (pfds[i].revents & (POLLIN|POLLOUT|POLLERR|POLLHUP)) {
                    Proc *p = vm->procs[pids[i]];
                    if (p && p->state == PROC_WAIT_IO) {
                        p->state = PROC_RUNNING;
                        runq_enqueue(vm, p->pid);
                    }
                }
            }
        } else {
            usleep(1000);  // no WAIT_IO actors, brief sleep
        }
    }
    return NULL;
}
```

### 2. worker_loop 简化

多线程 Phase 2 中**删除**所有 I/O poll 逻辑（CAS, io_polling, poll, usleep）。改为：

```c
// 多线程 Phase 2 (simplified)
if (atomic_load(&vm->active_procs) == 0) {
    vm->stop = 1;
    pthread_cond_broadcast(&vm->rq_cond);
    break;
}

// 检查死锁：runq 空 + 无 WAIT_IO + 无 busy worker
// 这个检查可以移到 poller 线程或保留在 worker 中
if (atomic_load(&vm->rq_count) == 0 &&
    atomic_load(&vm->busy_workers) == 0) {
    // 检查是否有 WAIT_IO actor
    int has_wait_io = 0;
    for (int i = 0; i < vm->procs_cap; i++) {
        if (vm->procs[i] && vm->procs[i]->state == PROC_WAIT_IO) {
            has_wait_io = 1;
            break;
        }
    }
    if (!has_wait_io) {
        vm->stop = 1;
        pthread_cond_broadcast(&vm->rq_cond);
        break;
    }
}

usleep(1000);  // brief sleep, I/O poller will wake us via runq_enqueue
```

**注意**：死锁检测逻辑需要小心。如果所有剩余 actor 都是 WAIT_IO，poller 线程正在等它们，这是正常的（不是死锁）。只有当所有剩余 actor 是 WAIT_RECV（等消息）且没有 busy worker 时才是死锁。

### 3. vm_run 启动 poller 线程

```c
void vm_run(VM *vm) {
    atomic_store(&vm->active_procs, 1);
    atomic_store(&vm->busy_workers, 0);
    vm->stop = 0;

    if (vm->nworkers <= 1) {
        WorkerCtx wc = { ... };
        worker_loop(&wc);
        return;
    }

    // Start I/O poller thread
    pthread_t io_thread;
    pthread_create(&io_thread, NULL, io_poller_thread, vm);

    // Start N workers
    vm->workers = malloc(vm->nworkers * sizeof(pthread_t));
    WorkerCtx *wctxs = malloc(vm->nworkers * sizeof(WorkerCtx));
    for (int i = 0; i < vm->nworkers; i++) {
        wctxs[i] = (WorkerCtx){ .vm = vm, .current_proc = NULL, .thread_id = i };
        pthread_create(&vm->workers[i], NULL, worker_thread_entry, &wctxs[i]);
    }

    // Join workers
    for (int i = 0; i < vm->nworkers; i++)
        pthread_join(vm->workers[i], NULL);

    // Stop and join poller
    vm->stop = 1;  // workers already set this, but be safe
    pthread_join(io_thread, NULL);

    free(wctxs);
}
```

### 4. 删除 io_polling atomic

`static atomic_int io_polling = 0;` 可以删除（不再需要 CAS 竞争）。

### 5. 单线程模式不变

单线程模式（nworkers <= 1）保留原有逻辑：worker_loop 中内嵌 I/O poll（Phase 2 中的 poll + wake）。这是正确的行为——单线程时不需要额外的 poller 线程。

## Files to Modify

- `src/vm.c` — io_poller_thread, worker_loop 简化, vm_run 启动 poller
- `ta.h` — 如果需要 io_poller_thread 声明

## Important Constraints

1. **nworkers=1 零回归** — 单线程模式完全不变
2. **GC 零改动**
3. **无死锁** — poller 和 worker 协调正确
4. **Skynet 不变量** — actor 同一时刻只在一个 worker 上

## Verification

```bash
# 1. Build
make clean && make

# 2. 单线程
NWORKERS=1 pass=0; fail=0; for f in test/scripts/*.lisp; do NWORKERS=1 timeout 10 ./tinyactor "$f" >/dev/null 2>&1; [ $? -eq 0 ] && pass=$((pass+1)) || fail=$((fail+1)); done; echo "ST: PASS=$pass FAIL=$fail"

# 3. 多线程
NWORKERS=4 pass=0; fail=0; for f in test/scripts/*.lisp; do NWORKERS=4 timeout 10 ./tinyactor "$f" >/dev/null 2>&1; [ $? -eq 0 ] && pass=$((pass+1)) || fail=$((fail+1)); done; echo "MT: PASS=$pass FAIL=$fail"

# 4. Concurrent + echo
NWORKERS=4 timeout 15 ./tinyactor example/scripts/concurrent_test.lisp; echo "EXIT=$?"

# 5. Echo server multi-thread
NWORKERS=4 timeout 5 ./tinyactor example/scripts/echo_server.lisp &
sleep 2; echo "poller-test" | nc -w 2 localhost 8090; pkill -f tinyactor

# 6. Stability
for i in $(seq 10); do NWORKERS=4 timeout 10 ./tinyactor example/scripts/concurrent_test.lisp >/dev/null 2>&1 || echo "FAIL run $i"; done; echo "10x done"
```

## Rules
1. nworkers=1 zero regression
2. GC untouched
3. No deadlock
4. Build must pass
5. Output DONE: <file list> when complete