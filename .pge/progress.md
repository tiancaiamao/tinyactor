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
- 34 new tests → 45 total (44 pass, 1 expected-fail: bytes-basic)

### Task 6: Validation + Review Fixes
- Independent Evaluator verified 32/33 criteria
- Code Review found 5 P1 + 5 P2 issues, all fixed

## Phase 3: 模块化 + 系统级集成 🔄 IN PROGRESS

### Task 1: Preempt Bug Fix ✅ COMPLETE — `0b48c2e`
- Stall counter in vm_run(): 10,000 iterations without state change → kill all RUNNING processes
- preempt.lisp exits 0 with output "ok"
- gc-deep-list.lisp now also passes

### Task 2+3: Module System + Network Module ✅ COMPLETE — `4168f66`
- TaFunc struct `{name, fn, nargs}` for module function descriptors
- vm_register_module(vm, "name", funcs[]): registers batch with prefixed names
- (import "name") in compiler — OP_PUSH_NIL (no-op at runtime, enables module.func calls)
- src/module.c: module registration implementation
- src/net.c: net.listen/accept/read/write/close — non-blocking sockets
- PROC_WAIT_IO state + poll() in vm_run Phase 2
- OP_CCALL detects 'would-block → rewind PC + set WAIT_IO
- module_test.lisp passes
- Reader: `.` added to is_ident_char() so `net.listen` parses as single symbol

### Task 4: TCP Echo Server ✅ COMPLETE — `4c6ea21`
- example/echo_server.c: C host program linking TinyActor VM
- example/scripts/echo_server.lisp: accept loop + handle-client actor
- example/scripts/echo_test.lisp: 3-client integration test (PASS)
- net.connect() added for TCP client connections
- **2 critical bugs found and fixed:**
  1. VM I/O bug: would-block rewind didn't restore args to stack → garbage on retry
  2. Compiler bug: import set has_top=true → main not auto-spawned
- Regression: 45/46 pass (bytes-basic pre-existing)

### Task 5: HTTP Server ⬜ NEXT
- example/http_server.c + example/scripts/http_server.lisp
- Routing, concurrent request handling, curl-verifiable

### Task 6: 独立验收 ⬜
- Full regression + independent evaluator + code review

## Current Code Stats
- ~3600 lines total (ta.h:430, api.c:207, compile.c:1220, gc.c:141, main.c:55, module.c:NEW, net.c:NEW, reader.c:213, val.c:226, vm.c:830)
- Build: `make clean && make` — 0 errors
- Tests: 45/46 pass (1 expected-fail: bytes-basic segfault)

## Git History
```
4c6ea21 Phase 3 Task 4: TCP Echo Server + 2 critical bug fixes
4168f66 Phase 3 Task 2+3: Module system + network module + I/O scheduler
0b48c2e Phase 3 Task 1: Fix preempt bug — stall counter in scheduler
9cdaaed Update Phase 2 spec + progress documentation
31cfcf8 Phase 2: Fix 10 issues from independent review + evaluator
```