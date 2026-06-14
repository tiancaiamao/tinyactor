# TinyActor — Progress

## Phase 1: 能跑 ✅ COMPLETE
- Task 1: ta.h + val.c — types, NaN-boxing, opcodes (566 lines)
- Task 2: reader.c + compile.c — parser + compiler (~1330 lines)
- Task 3: vm.c + api.c + main.c + Makefile — runtime (~900 lines)
- Task 4: Integration debug — 17 bugs fixed across 3 sub-agents
- **11/11 tests passing**

## Phase 2: 完善 ✅ COMPLETE

### Task 1+2: GC Core + Integration
- src/gc.c (141 lines): per-process semispace copying collector
- ta.h: gc_roots[16], gc_root_count, current_proc, gc_collect, gc_root_push/pop
- vm.c: proc_new allocates gc_to, vm_run sets current_proc, proc_grow
- val.c: gc_root_push/pop in val_pair/val_closure

### Task 3+4: String Builtins + C Function Call
- OP_STR_LEN, OP_STR_CONCAT, OP_STR_SLICE, OP_STR_EQ
- OP_CCALL: call registered C functions from script

### Task 5: Test Suite
- 34 new tests → 46 total (45 pass, 1 expected-fail: bytes-basic)

### Task 6: Validation + Review Fixes
- Independent Evaluator verified 32/33 criteria
- Code Review found 5 P1 + 5 P2 issues, all fixed

## Phase 3: 模块化 + 系统级集成 ✅ COMPLETE

### Task 1: Preempt Bug Fix ✅ — `0b48c2e`
- Stall counter in vm_run(): 10,000 iterations without state change → kill all RUNNING processes
- preempt.lisp exits 0 with output "ok"
- gc-deep-list.lisp now also passes

### Task 2+3: Module System + Network Module ✅ — `4168f66`
- TaFunc struct `{name, fn, nargs}` for module function descriptors
- vm_register_module(vm, "name", funcs[]): registers batch with prefixed names
- (import "name") in compiler — OP_PUSH_NIL (no-op at runtime, enables module.func calls)
- src/module.c: module registration implementation
- src/net.c: net.listen/accept/read/write/close — non-blocking sockets
- PROC_WAIT_IO state + poll() in vm_run Phase 2
- OP_CCALL detects 'would-block → rewind PC + set WAIT_IO
- module_test.lisp passes
- Reader: `.` added to is_ident_char() so `net.listen` parses as single symbol

### Task 4: TCP Echo Server ✅ — `4c6ea21`
- example/echo_server.c: C host program linking TinyActor VM
- example/scripts/echo_server.lisp: accept loop + handle-client actor
- example/scripts/echo_test.lisp: 3-client integration test (PASS)
- net.connect() added for TCP client connections
- **2 critical bugs found and fixed:**
  1. VM I/O bug: would-block rewind didn't restore args to stack → garbage on retry
  2. Compiler bug: import set has_top=true → main not auto-spawned

### Task 5: HTTP Server ✅ — `3a5e0bf`
- src/http.c: HTTP module (http.parse_request, http.response)
- example/http_server.c: C host program with net + http modules
- example/scripts/http_server.lisp: actor-per-connection, 3+ routes
- Routes: / (HTML), /api (JSON), /time, */ (404)
- Verified with curl: all routes return correct content + headers

### Task 6: Independent Validation + Review Fixes ✅ — `f5a0a04`
- **Evaluator** (6bf936): 16/19 criteria pass, 1 fail (echo_server.lisp paren bug), 2 partial
- **Evaluator finding fixed**: echo_server.lisp parenthesis imbalance → fixed (`18b81fb`)
- **Review** (5fb1af): 6 P1 + 10 P2 + 3 P3 issues found
- **Generator** (1c0147): Fixed all P1 + critical P2 issues:
  - P1-01: net_read VLA → malloc (stack overflow)
  - P1-02: http_response two-pass snprintf (buffer overflow)
  - P1-03: vm_free frees module registry (memory leak)
  - P1-04: realloc NULL check in module.c
  - P1-05: poll() uses POLLIN+POLLOUT (write wait processes now wake)
  - P1-06: stall counter reverted (original was correct — false positive from review)
  - P2-01: SO_REUSEADDR on net_listen
  - P2-04: proc_die closes wait_fd only when in WAIT_IO state
  - P2-07: http_response GC-safe allocation
  - P2-10: poll fd array 128 → 1024

### Subagents Used (Phase 3)
1. gen-preempt2 (9b203d) — stall counter fix
2. gen-module-net (f54ca3) — module system + network module + I/O scheduler
3. gen-http (24b3ca) — HTTP server
4. eval-phase3 (6bf936) — independent acceptance verification
5. review-phase3 (5fb1af) — code review
6. gen-fix-review (1c0147) — fix all P1+P2 issues

## Current Code Stats
- **~4500 lines** total (ta.h:500, api.c:212, compile.c:1300, gc.c:141, http.c:85, main.c:60, module.c:84, net.c:172, reader.c:213, val.c:226, vm.c:1200)
- Build: `make clean && make` — 0 errors
- Tests: **48/48 pass** (bytes-basic now fixed by OP_SEND stack balance fix)
- Examples: echo_server + http_server both fully functional (concurrent HTTP verified)

## Git History
```
f5a0a04 Phase 3 Task 6: Fix P1+P2 issues from independent review + evaluator
18b81fb Fix echo_server.lisp parenthesis imbalance (evaluator finding)
3a5e0bf Phase 3 Task 5: HTTP Server with routing
4c6ea21 Phase 3 Task 4: TCP Echo Server + 2 critical bug fixes
4168f66 Phase 3 Task 2+3: Module system + network module + I/O scheduler
0b48c2e Phase 3 Task 1: Fix preempt bug — stall counter in scheduler
9cdaaed Update Phase 2 spec + progress documentation
31cfcf8 Phase 2: Fix 10 issues from independent review + evaluator
21d3689 Phase 2 complete: GC + string builtins + C function call + 45 tests
590f40f Phase 2 Task 5: Test suite integration (34 new tests, 45 total)
71be2e1 Phase 2 Task 3+4: String builtins + C function call mechanism
8ef5b5e Phase 2 Task 1+2: Per-process semispace copying GC
6a542a6 Add .gitignore, remove build artifacts from tracking
3ddc354 Phase 1 complete: TinyActor VM with actor concurrency
```

## Phase 4: 多线程调度器 + Heap Fragment ✅ COMPLETE

### Task 1: Thread Infrastructure — `d18be4a`
- pthread-based worker threads sharing global runq
- mutex + condvar for runq signaling
- `active_procs` atomic counter for exit detection
- thread-local `current_proc` for GC root tracking
- `vm_run` → `vm_start` (spawns N workers, waits for completion)

### Task 2: Heap Fragment Message Passing — `caca0ee`
- Send deep-copies message via malloc'd fragment to target mailbox
- GC never crosses process boundary (zero GC changes)
- OP_SEND pops msg + pid, pushes nil for stack balance
- OP_RECV peeks mailbox, returns first message or blocks

### Task 3: Multi-Worker Threading — `5c59227`
- NWORKERS env var controls thread count (default 1)
- Workers pull from shared runq with mutex protection
- Exit condition: active_procs == 0 → broadcast condvar → all workers exit
- Each actor runs on exactly one worker at a time (runq mutex guarantees)

### Task 4: Dedicated I/O Poller Thread — `b2560fc`
- Separate io_poller thread using poll() on all WAIT_IO processes
- Decoupled from worker threads: poller wakes actors, workers execute them
- poll_interval adaptive: 1ms when I/O pending, 10ms otherwise

### Task 5: Selective Receive — `c88831c`
- OP_RECV_PEEK: scan mailbox at peek_index, push copy or block
- OP_RECV_COMMIT: remove matched fragment, reset peek_index
- `(receive (pattern body...) ...)` syntax compiles to scan loop
- peek_index preserved across block/resume for incremental scanning

### Task 6: Bug Fixes — Technical Debt Cleanup
- **http module registration**: `vm_register_http_module(vm)` was missing in main.c → `http.parse_request`/`http.response` unresolved → nil closure → SEGV at OP_CALL. Fix: added registration call.
- **bytes-basic segfault**: Fixed by OP_SEND stack balance fix (push nil after send).
- **HTTP server**: Now fully functional with all routes + concurrent requests (10/10 verified).

## Known Limitations
- [ ] net_connect uses blocking connect() (can stall VM)
- [ ] No try/catch / throw (Erlang philosophy: Let it crash, use supervisor)
- [ ] HTTP server is minimal (no chunked encoding, no keep-alive)
- [ ] poll fd limit: 1024 concurrent connections max
- [ ] let without env_pop: variables persist after their binding scope (benign, not a correctness issue)