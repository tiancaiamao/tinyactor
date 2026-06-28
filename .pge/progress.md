# Progress Tracker

## Status: Phases 1-6 COMPLETE with real test coverage

### Test Results: 83 tests total
- 60 original tests (C compiler path) — all pass
- 11 bootstrap tests (--bootstrap mode, Lisp pipeline) — all pass (except flaky TCP echo_test)
- 1 self-hosting test (bootstrap → compile driver.ta → run) — passes
- 11 bytecode comparison tests (C vs Lisp output equivalence) — all pass

### Phase 1: codegen.lisp basic ✅
### Phase 2: codegen.lisp full features ✅
### Phase 3: parser.ta pattern desugar ✅
### Phase 4: VM multi-module loading ✅
### Phase 5: bootstrap driver ✅
### Phase 6: self-hosting bootstrap ✅
### Phase 7: MATCH_* opcode removal (OPTIONAL, DEFERRED)

### Bugs Found and Fixed (during test coverage audit)
1. **Import resolution**: driver.ta compile_file didn't resolve imports → used vm.load_source
2. **receive compilation**: codegen.lisp didn't handle `receive { }` blocks → added compile_receive
3. **Pattern matching**: missing quoted symbol, cons, and list pattern compilation
4. **Slot allocation**: off-by-one in pair pattern (cdr_slot vs cdr_slot-1)
5. **OP_MATCH_SYM rebasing**: api.c didn't remap match symbol indices during module loading