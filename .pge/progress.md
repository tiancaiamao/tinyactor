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
  - gc_collect: root scan (stack, mailbox, gc_roots) → copy → scan tospace → swap
  - Forwarding: HeapHeader.flags bit 0 + memcpy for forwarding pointer
  - Trigger: proc_heap_alloc failure → GC → retry → proc_grow → crash
  - gc_collect heap-stack collision check before swap (review fix)
- ta.h: gc_roots[16], gc_root_count, current_proc, gc_collect, gc_root_push/pop
- vm.c: proc_new allocates gc_to, vm_run sets current_proc, proc_grow
- val.c: gc_root_push/pop in val_pair/val_closure (review fix — P1 critical)

### Task 3+4: String Builtins + C Function Call
- OP_STR_LEN, OP_STR_CONCAT, OP_STR_SLICE, OP_STR_EQ
  - Type checks on all string ops (review fix)
  - GC-safe: malloc for large concats (review fix)
- OP_CCALL: call registered C functions from script
  - args[64] + bounds checks on nargs and cfidx (review fix)
- compile.c: string ops in inline_ops + cfunc dispatch in cx_call
- api.c: vm_register() with strdup NULL check (review fix)

### Task 5: Test Suite
- 34 new tests from cora-inspired analysis → 45 total
- Categories: GC stress, GC retention, GC+Actor, closures, tail calls, strings, lists, patterns, errors, integration

### Task 6: Validation + Review Fixes
- Independent Evaluator verified 32/33 criteria (d15eda)
- Code Review found 5 P1 + 5 P2 issues (7e9ebe)
- Generator fixed all 10 issues (02a6ef)
- Final: 42/45 pass (3 expected-fail: bytes not impl, gc-deep-list capacity, preempt timeout)

### Subagents Used (Phase 2)
1. explore-tests (5cc423) — cora test analysis, designed 34 tests
2. explore-interop (769cfd) — cora/Lua C interop analysis, GC+C coupling design
3. gen-gc2 (8c24cf) — GC core implementation
4. gen-builtins (1bb3b5) — string builtins + OP_CCALL
5. gen-tests (a2bf71) — test suite integration + syntax fixes
6. eval-phase2 (d15eda) — independent acceptance criteria verification
7. review-phase2 (7e9ebe) — code review
8. gen-fix-review (02a6ef) — fix all review + evaluator findings

## Phase 3: 模块化 + 系统级集成 🔄 IN PROGRESS

### Task 1: Preempt Bug Fix（Phase 2 遗留）
- [ ] 根进程退出后清理所有子进程
- [ ] preempt.lisp exit 0 + 输出 "ok"

### Task 2: 模块系统
- [ ] TaModule 结构体 + vm_register_module
- [ ] (import "name") 编译器支持
- [ ] module.func 调用解析
- [ ] module_test.lisp 测试

### Task 3: 网络 C 模块 + I/O 调度器
- [ ] src/net.c: net.listen/accept/read/write/close
- [ ] PROC_WAIT_IO 状态
- [ ] 调度器 poll() 集成
- [ ] non-blocking socket

### Task 4: TCP Echo Server 示例
- [ ] example/echo_server.c
- [ ] echo_server.lisp (server + client actors)
- [ ] 真实 TCP 交互验证

### Task 5: HTTP Server 示例
- [ ] example/http_server.c
- [ ] http_server.lisp (路由 + 并发 handler)
- [ ] curl 验证

### Task 6: 独立验收
- [ ] Phase 2 回归 42 pass
- [ ] Evaluator 独立验证
- [ ] Review 代码审查

## Current Code Stats
- **3265 lines** total (ta.h:405, api.c:207, compile.c:1217, gc.c:141, main.c:42, reader.c:213, val.c:226, vm.c:814)
- Build: `make clean && make` — 0 errors, 2 unused function warnings
- Tests: 42/45 pass, 3 expected-fail (bytes-basic segfault, gc-deep-list timeout, preempt no-exit)

## Git History
```
9cdaaed Update Phase 2 spec + progress documentation
31cfcf8 Phase 2: Fix 10 issues from independent review + evaluator
21d3689 Phase 2 complete: GC + string builtins + C function call + 45 tests
590f40f Phase 2 Task 5: Test suite integration (34 new tests, 45 total)
71be2e1 Phase 2 Task 3+4: String builtins + C function call mechanism
8ef5b5e Phase 2 Task 1+2: Per-process semispace copying GC
6a542a6 Add .gitignore, remove build artifacts from tracking
3ddc354 Phase 1 complete: TinyActor VM with actor concurrency
```