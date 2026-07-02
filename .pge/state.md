# PGE State

## Active Task
None — all phases complete.

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