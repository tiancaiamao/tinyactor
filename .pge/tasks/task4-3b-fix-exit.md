# Task: Phase 4 Task 3b-fix — 多线程退出条件修复

## Context

多线程 vm_run 已经实现（Task 3b），但有两个测试在多线程下超时：
- `many_actors.lisp`：1000 个 actor 卡在 WAIT_RECV，active_procs 永远 > 0
- `actor-selective-recv.lisp`：main actor 卡在 WAIT_RECV

**根因**：多线程退出条件 `active_procs == 0` 太严格。单线程退出条件是 `nfds == 0 → break`（没有 WAIT_IO 就退出）。但多线程下，如果所有剩余 actor 都是 WAIT_RECV（等消息），没有人在执行可以 send，那就是死锁——应该退出。

但有个竞态：另一个 worker 可能正在执行一个会 send 的 actor。所以不能只检查 runq 空 + nfds==0，还需要确认没有 worker 正在执行。

## Fix

### 1. 添加 busy_workers 计数器

在 `VM` 结构体（ta.h）中添加：
```c
atomic_int busy_workers;  /* number of workers currently executing an actor */
```

在 `src/vm.c` worker_loop 中，执行 actor 前后增减：

```c
// 在 vm_step 循环之前
atomic_fetch_add(&vm->busy_workers, 1);
tls_current_proc = p;
wc->current_proc = p;
for (int r = 0; r < MAX_REDUCTIONS; r++) {
    if (vm_step(vm, p) != 0) break;
}
atomic_fetch_sub(&vm->busy_workers, 1);
```

在 `vm_run` 中初始化：
```c
atomic_store(&vm->busy_workers, 0);
```

### 2. 修改多线程退出条件

在 worker_loop 的多线程 Phase 2 中，poller 收集到 nfds==0 时：

```c
// 当前代码（有问题）：
if (nfds == 0) {
    usleep(1000);  // 永远不退出！
}

// 改为：
if (nfds == 0) {
    /* No actors waiting on I/O. If runq is also empty AND no worker
     * is executing an actor, all remaining actors are WAIT_RECV →
     * no one can send → deadlock → exit. */
    if (atomic_load(&vm->rq_count) == 0 &&
        atomic_load(&vm->busy_workers) == 0) {
        vm->stop = 1;
        pthread_cond_broadcast(&vm->rq_cond);
        break;
    }
    usleep(1000);  /* brief wait for running actors to finish */
}
```

**为什么 busy_workers == 0 是安全的？**
- busy_workers > 0：另一个 worker 正在执行 actor，可能会 send → 不退出
- busy_workers == 0：所有 worker 都在 Phase 2（睡眠或 poll），没有 actor 在执行 → 不可能有新的 send → 死锁 → 安全退出

### 3. 同样修复非 poller 的 usleep 分支

非 poller 的 worker 在 usleep 后应该也检查是否应该退出（通过 vm->stop 已有检查）。

## Files to Modify

- `ta.h` — 添加 `atomic_int busy_workers` 到 VM 结构体
- `src/vm.c` — 初始化 busy_workers, 增减计数, 修改退出条件
- `src/api.c` — 可能在 vm_new 中初始化（如果 vm_new 初始化其他 atomic 字段的话）

## Verification

```bash
# 1. Build
make clean && make

# 2. 单线程零回归
NWORKERS=1 pass=0; fail=0; for f in test/scripts/*.lisp; do NWORKERS=1 timeout 10 ./tinyactor "$f" >/dev/null 2>&1; if [ $? -eq 0 ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL: $(basename $f)"; fi; done; echo "PASS: $pass FAIL: $fail"

# 3. 多线程全部测试（关键！）
NWORKERS=4 pass=0; fail=0; for f in test/scripts/*.lisp; do NWORKERS=4 timeout 10 ./tinyactor "$f" >/dev/null 2>&1; if [ $? -eq 0 ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL: $(basename $f)"; fi; done; echo "PASS: $pass FAIL: $fail"

# 4. Multi-thread concurrent
NWORKERS=4 timeout 15 ./tinyactor example/scripts/concurrent_test.lisp; echo "EXIT=$?"

# 5. 10x stability
for i in $(seq 10); do NWORKERS=4 timeout 10 ./tinyactor example/scripts/concurrent_test.lisp >/dev/null 2>&1 || echo "FAIL run $i"; done
```

## Rules
1. nworkers=1 零回归
2. 所有 46 个测试在 NWORKERS=4 下必须通过（bytes-basic pre-existing fail OK）
3. GC 不能修改
4. Build must pass
5. Output DONE: <file list> when complete