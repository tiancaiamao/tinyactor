# Spec: TinyActor Phase 4 — 多线程调度器 + Heap Fragment + Selective Receive

## Goal
将 TinyActor 从单线程 VM 升级为 Skynet 风格的多线程 actor 调度器：N 个 worker 线程共享一个全局就绪队列，每个 actor 同一时刻只在一个线程上执行，消息传递通过 heap fragment 实现线程安全的 GC 隔离。

## Background — 架构决策（已确认）

| 决策 | 选择 | 理由 |
|------|------|------|
| 多线程模型 | 单 VM 内多 worker 线程 (Skynet 风格) | actor 通信透明，Skynet 生产验证过 |
| 调度队列 | 单全局 runq + mutex + condvar | 简单可靠，2-8 核无瓶颈 |
| Work stealing | 不做 | 复杂度过高，目标 2-8 核不需要 |
| GC 安全 | Heap Fragment (BEAM 做法) | send 不碰目标堆，GC 零改动 |
| I/O poller | 独立线程 | 与 worker 解耦 |
| Selective receive | 实现 | actor 邮箱扫描匹配，Erlang 核心语义 |

## 核心不变量

**一个 actor 同一时刻只在一个 worker 线程上执行。**

这条规则消除了 90% 的锁需求：actor 执行（vm_step）、GC（gc_collect）、栈/堆操作全部无需锁。只需锁全局 runq 和每个 actor 的 mailbox。

---

## Acceptance Criteria

### L1 — Structural

- [ ] `make clean && make` 零 error 编译，链接 `-lpthread` — Verify: `make clean && make 2>&1`
- [ ] `ta.h` 包含 `MsgFragment` 结构体定义 — Verify: `grep MsgFragment ta.h`
- [ ] `ta.h` 中 `Proc` 有 `pthread_mutex_t mbox_lock` 字段 — Verify: `grep mbox_lock ta.h`
- [ ] `ta.h` 中 `VM` 有 `pthread_mutex_t rq_lock` 和 `pthread_cond_t rq_cond` — Verify: `grep 'rq_lock\|rq_cond' ta.h`
- [ ] `VM` 有 `atomic_int next_pid` 和 `atomic_int active_procs` — Verify: `grep 'atomic_int.*next_pid\|atomic_int.*active_procs' ta.h`
- [ ] `VM` 有 `nworkers` 和 `stop` 字段 — Verify: `grep 'nworkers\|vm->stop' ta.h`
- [ ] `vm->current_proc` 不再存在（改为 thread-local 或 WorkerCtx） — Verify: `grep 'current_proc' ta.h | grep -v gc_root` (should be empty or in WorkerCtx only)
- [ ] `procs[]` 预分配为固定大小 `MAX_PROCS`，无运行时 realloc — Verify: `grep MAX_PROCS ta.h`
- [ ] Phase 3 回归：45/46 测试仍通过（bytes-basic pre-existing fail） — Verify: `for f in test/scripts/*.lisp; do timeout 10 ./tinyactor "$f"; done; echo done`

### L2 — Behavioral

#### Task 1+2: 基础线程设施 + Heap Fragment（单线程验证）

- [ ] **L2.1 nworkers=1 回归**：所有 46 个测试在单 worker 模式下全部通过 — Verify: `for f in test/scripts/*.lisp; do timeout 10 ./tinyactor "$f" >/dev/null 2>&1 || echo "FAIL: $f"; done`
- [ ] **L2.2 Fragment 传递正确**：send/recv 消息内容不变 — Verify: 创建 `test/scripts/multithread-basic.lisp`，3 个 actor 互发 string/pair/symbol 消息，收集结果打印 PASS — Verify: `timeout 10 ./tinyactor test/scripts/multithread-basic.lisp`
- [ ] **L2.3 Echo server fragment 模式正常**：echo_server + echo_test 仍 PASS — Verify: `timeout 15 ./tinyactor example/scripts/echo_test.lisp`
- [ ] **L2.4 Concurrent test fragment 模式正常**：5 并发 client ALL PASS — Verify: `timeout 15 ./tinyactor example/scripts/concurrent_test.lisp`

#### Task 3: 多 Worker 线程

- [ ] **L2.5 多线程 echo server**：2+ workers 下 echo server 正常响应 — Verify: `NWORKERS=4 timeout 5 example/echo_server &` + `echo test | nc localhost 8090`
- [ ] **L2.6 多线程 HTTP server**：2+ workers 下 HTTP server 三条路由正确 — Verify: `NWORKERS=4 example/http_server &` + `curl -s localhost:8080/`
- [ ] **L2.7 多线程 concurrent test**：4 workers 下 5 并发 client ALL PASS — Verify: `NWORKERS=4 timeout 15 ./tinyactor example/scripts/concurrent_test.lisp`
- [ ] **L2.8 VM 自然退出**：所有 actor 死亡后 VM 正常退出（exit 0），不死锁不 hang — Verify: `timeout 15 ./tinyactor example/scripts/concurrent_test.lisp; echo "EXIT=$?"` (EXIT=0)
- [ ] **L2.9 无竞态 crash**：连续跑 10 次 concurrent_test 不 crash — Verify: `for i in $(seq 10); do NWORKERS=4 timeout 10 ./tinyactor example/scripts/concurrent_test.lisp || echo "FAIL run $i"; done`

#### Task 4: I/O Poller 独立线程

- [ ] **L2.10 I/O poller 与 worker 解耦**：HTTP server 在 4 workers 下，多个并发 curl 请求全部正确返回 — Verify: `for i in $(seq 5); do curl -s http://localhost:8080/ & done; wait`
- [ ] **L2.11 延迟连接正常**：server 启动后等 2 秒再连接，仍正常响应 — Verify: 启动 server, sleep 2, curl

#### Task 5: Selective Receive

- [ ] **L2.12 Selective receive 语法**：`(receive (('ping from) ...) (msg ...))` 编译通过 — Verify: 创建 `test/scripts/recv-scan.lisp` 并运行
- [ ] **L2.13 邮箱扫描匹配**：收到多条消息时，selective receive 跳过不匹配的，只取匹配的 — Verify: `timeout 10 ./tinyactor test/scripts/recv-scan.lisp` 输出 PASS
- [ ] **L2.14 不匹配消息保留**：被 selective receive 跳过的消息仍留在邮箱，后续 recv 可取 — Verify: 测试脚本中先 selective receive 取特定消息，再普通 recv 取剩余消息
- [ ] **L2.15 无匹配则阻塞**：邮箱中没有匹配消息时，actor 挂起（PROC_WAIT_RECV），有匹配消息时自动唤醒 — Verify: 测试脚本中 actor A 等 B 发特定消息，B 先发不匹配的再发匹配的

## Constraints

- **C99 + pthreads**：不依赖 C11 `<threads.h>`，用 `<pthread.h>`
- **GC 零改动**：`gc_collect()` 和 `gc_copy_val()` 的 semispace copy 逻辑不能改。fragment 是 malloc'd 内存，GC 不扫它。
- **单 VM 实例**：所有 actor 在同一个 VM 内，互相通信透明
- **Skynet 不变量**：一个 actor 同一时刻只在一个 worker 线程上执行
- **不破坏现有 API**：`vm_new() / vm_load() / vm_run() / vm_free()` 签名不变（`vm_new` 可接受 worker 数参数或用默认值）
- **nworkers=1 必须工作**：退化为单线程时行为与 Phase 3 完全一致
- **平台**：macOS + Linux（pthread 两者都支持）

## Out of Scope

- **Work stealing**（per-thread 队列 + 偷取）— 推到未来 Phase
- **分代 GC** — 推到未来 Phase
- **try/catch / throw** — 推到自举之后
- **语言完整性**（嵌套 pattern matching、guard、更多字符串操作）— 推到自举之后
- **epoll/kqueue 替代 poll** — 当前 poll 足够
- **多 VM 实例 / 跨 VM 通信** — 路线 A，未来考虑
- **实用性扩展**（文件 I/O、定时器、更完整 HTTP）— 不在本 Phase

## Implementation Plan

详细设计见 `.pge/phase4-design.md`。

### Task 顺序（有依赖关系，必须串行）

| Task | 内容 | 依赖 | 预估行数 |
|------|------|------|----------|
| 1 | 基础线程设施：ta.h 结构体改造 + runq 加锁 + vm_run 启动线程 + nworkers=1 回归 | 无 | ~150 |
| 2 | Heap Fragment：MsgFragment + mailbox 改造 + send/recv/proc_die 改用 fragment | Task 1 | ~150 |
| 3 | 多 Worker 线程：worker_loop + thread-local current_proc + nworkers>1 | Task 1+2 | ~100 |
| 4 | I/O Poller 独立线程：从 vm_run 分离为 io_poller_loop | Task 3 | ~60 |
| 5 | Selective Receive：compile.c receive 语法 + OP_RECV_SCAN | Task 1-4 | ~150 |
| 6 | 技术债清理 + 文档更新 | Task 1-5 | ~80 |

### 关键设计点

1. **Heap Fragment**：send 时 malloc 一块 fragment 内存，把消息深拷贝进去，挂到目标 mailbox 链表。recv 时从 fragment 反序列化到自己的堆，然后 free fragment。GC 不需要改。
2. **procs[] 固定大小**：预分配 MAX_PROCS=65536 个 slot（512KB），spawn 只写入不 realloc。
3. **退出条件**：`active_procs` 原子计数器，所有 actor 死亡后 broadcast condvar 唤醒 worker 退出。
4. **I/O poller 安全**：poller 只读 `proc->state == PROC_WAIT_IO` 并设为 RUNNING，不碰 actor 内部数据。
5. **Selective receive**：扫描 mailbox fragment 链表，第一条匹配的消息摘除并返回，不匹配的保留。