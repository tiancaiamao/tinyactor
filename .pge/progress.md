# TinyActor — Progress

## Phase 1: 能跑 ✅ COMPLETE
- Task 1: ta.h + val.c (types, NaN-boxing, opcodes)
- Task 2: reader.c + compile.c (parser + compiler)
- Task 3: vm.c + api.c + main.c + Makefile (runtime)
- Task 4: Integration debug (17 bugs fixed across 3 sub-agents)
- 11/11 tests passing

## Phase 2: 完善 ✅ COMPLETE

### Task 1+2: GC Core + Integration
- [x] src/gc.c (~122 lines): semispace copying collector
  - gc_collect: root scan → copy → scan tospace → swap
  - Roots: stack, mailbox, gc_roots, catch_stack
  - Forwarding: HeapHeader.flags bit 0 + pointer after header
  - GC trigger: proc_heap_alloc failure → GC → retry → grow → crash
- [x] ta.h: gc_roots[16], gc_root_count, current_proc, gc_collect, gc_root_push/pop
- [x] vm.c: proc_new allocates gc_to, vm_run sets current_proc, proc_grow
- [x] val.c: val_tag moved to ta.h inline

### Task 3+4: String Builtins + C Function Call
- [x] OP_STR_LEN, OP_STR_CONCAT, OP_STR_SLICE, OP_STR_EQ
- [x] OP_CCALL: call registered C functions from script
- [x] compile.c: string ops in inline_ops + cfunc dispatch in cx_call
- [x] api.c: vm_register() implementation
- [x] GC-safe: extract C data before allocation in string ops

### Task 5: Test Suite
- [x] 34 new tests from cora-inspired analysis (45 total)
- [x] GC stress: pair-churn, closure-churn, string-churn, cross-reference
- [x] GC retention: stack-refs, free-vars, deep-list
- [x] GC+Actor: under-send, during-recv, multi-process, closure-in-spawn
- [x] Closures: curry, nested-capture, overwrite-scope, send-to-other-process
- [x] Tail calls: 5M iterations, mutual recursion, begin-in-tail
- [x] Strings: basic-ops, gc-stress, in-list
- [x] Lists: reverse, map, iterate-sum
- [x] Patterns: pair-destructure, match-in-actor
- [x] Error: crash-isolation, send-to-dead, supervisor-restart
- [x] Integration: fib+closure+actor, ping-pong-stress

### Task 6: Final Validation
- [x] L1: Build clean (2 unused function warnings only)
- [x] L2: GC correctness — 5/5 tests pass
- [x] L3: String operations — 3/3 tests pass
- [x] L4: C function mechanism — OP_CCALL + vm_register working
- [x] L5: Actor+GC integration — 3/3 tests pass
- [x] L6: Enhanced tests — all pass
- [x] Phase 1 regression — 11/11 still pass
- [x] Total: 45/45 tests pass (exit 0)

## Stats
- Total code: ~3205 lines (ta.h:397 + val.c:222 + reader.c:213 + compile.c:1217 + gc.c:122 + vm.c:787 + api.c:205 + main.c:42)
- Phase 2 added: ~262 lines net (gc.c:122, string ops ~80, OP_CCALL ~30, tests ~650)
- Build: `make clean && make` — 2 unused function warnings
- Subagents used: gen-gc2 (GC), gen-builtins (strings+cfunc), gen-tests (test suite)

## Commits
1. `3ddc354` — Phase 1 complete (11 tests)
2. `6a542a6` — .gitignore + cleanup
3. `8ef5b5e` — Phase 2 GC core
4. `71be2e1` — Phase 2 string builtins + C function call
5. `590f40f` — Phase 2 test suite (34 new tests, 45 total)