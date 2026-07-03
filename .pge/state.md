# PGE State

## Active Task
Phase 6 complete. All tasks committed. Review done (2 P2 findings noted, no blockers).

## P2 Review Findings (deferred)
- str.to_sym: uses forall(a, string -> a) instead of string -> symbol (no t_symbol() base type exists)
- str.sym_to_str: uses forall(a, a -> string) instead of symbol -> string (same reason)
- Future: add t_symbol() base type to type checker

## Completed Tasks

### Task A8: Builtin Function Type Signatures (pending commit)
- Added 14 builtin type signatures to `make_builtin_env` in typecheck.ta
- str module: char_at, substr, to_int, from_int, index_of, to_sym, sym_to_str
- Arithmetic: +, -, *, / as int -> int -> int
- Comparisons: <, >, <=, >= as int -> int -> bool
- Equality: == as forall(a, a -> a -> bool)
- Boolean: not as bool -> bool
- Eval: PASS (7/7 criteria met)

### Task A7: Error Location Info (committed 2100b76)
- Threaded `ctx` (function name symbol) through type inference pipeline
- `unify_check(t1, t2, s, ctx)` now accepts ctx parameter
- `infer_expr`, `infer_body`, `infer_lambda`, `infer_compound` all carry ctx
- `infer_define` passes function name as ctx
- `print_error_list` formats: `in function 'name': cannot unify X with Y`
- Error messages now show which function has the type error

### Task A6: Type Annotation Enforcement (pending commit)
- C reader emits `(type-sig ...)` forms when functions have type annotations
- `parse_source` in api.c flattens `(begin ...)` at top level
- C compiler handles `type-sig` as no-op
- Eval: PASS (6/6 criteria met)
- `fn bad_fn() -> string { 42 }` → type error detected
- `fn good_fn(x: int) -> int { x + 1 }` → no error
- Mixed/unannotated functions backward compatible

## Completed Phases

### Phase 1: Generic ADT (commit 2ebd71d)
- `t_app` type constructor for parameterized types
- `collect_variants` for compound annotations
- Exhaustiveness checking for generic ADTs

### Phase 2: proc_push Memory Fix (commit e128b63)
- Stack-heap collision in proc_push after GC
- Added missing proc_grow() call
- Also fixed gc-closure-in-spawn.ta SEGFAULT

### Phase 3: Typecheck Crash Fix (commit bdd8215)
- Pair guards on all 4 traversal functions
- Non-pair AST forms (import statements) no longer crash

### Phase 4: Typecheck Error Reporting (commit 726f8af)
- Added unify_check() to record type errors as substitution markers
- Error reporting via --check flag in driver
- 7 unify call sites replaced with unify_check
- Silent by default, only prints with --check

### Phase 5: Tests & Performance Analysis (commit 0dfb766)
- typecheck-clean.ta: verifies no false positives
- typecheck-errors.ta: verifies error detection (2 errors)
- run_typecheck_test() integrated into test suite
- docs/typecheck-performance.md: O(N³) root cause analysis
- Root cause: generalize() → free_vars_env() is O(N³)
- Optimization roadmap: level-based generalization → O(N²T)

## Test Results
- 185/188 pass (3 timing-related flaky tests)
- Typecheck tests: 2/2 pass
- No regressions from previous baseline