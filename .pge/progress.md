# PGE Progress — Real Programs Type Checking

## Current Status: Phase 1 COMPLETE ✅

### Completed
1. **Core HM inference** (commits 4177110, 01566b4, 671e1fe): unify, apply_subst, occur_check, fresh, infer_expr, generalize, instantiate, infer_program
2. **Actor primitives** (commit 3b6aa9d): spawn/send/recv/self/monitor/receive-scan builtins, t_pid(), str builtins
3. **receive-scan inference**: handles `(receive-scan (lambda (msg) body))` pattern
4. **Tests A-F**: All passing — spawn+send, self, receive-scan, if/else (match desugar), monitor, complete actor loop
5. **Bug discovered & worked around**: Parser has `items[64]` limit in `reader_ta.c` `parse_block_rest()` — function bodies >64 statements silently truncate, leaking trailing forms as top-level expressions. Workaround: split tests into separate functions.

### Test Results (lib/typecheck.ta)
```
Test 1-7: Basic HM inference ✅
Test A: spawn + send → 'a -> pid ✅
Test B: self() → 'a -> 'b -> pid ✅ (double arrow from 0-param + self value ref)
Test C: receive-scan → 'a -> 'b ✅ (polymorphic)
Test D: if/else (match desugar) → int -> int ✅
Test E: monitor → 'a -> pid ✅
Test F: complete actor → int -> int ✅
```

### Bootstrap: ✅ Verified

## Phase 2: ADT + Pattern Match — COMPLETE ✅
- `collect_type_decls`: processes `(type Name variants...)` forms
- Nullary variant `(quote V)`: registered as `(base Name)` in env
- N-ary variant `(Ctor f1 f2 ...)`: registered as `forall(ids, arrow_chain -> (base Name))`
- `infer_program` calls `collect_type_decls` before `collect_defines`
- `quote` in `infer_compound` resolves registered nullary constructors
- `type_format_resolved` renders ADT type names via `str.sym_to_str`
- Tests G/H/I: all pass (Color, Option, Pair)
- Eval report: PASS (.pge/eval-adt.md)
- Helper functions: `make_fresh_tvars`, `tvar_ids`, `build_ctor_arrow`, `collect_variants`, `collect_type_decls`, `t_base`

## Known Issues (from Phase 4 Review)
- **P2: Quote handler overly broad** — `infer_compound` quote branch resolves ALL env-bound quoted symbols, not just ADT constructors. E.g. `'x` after `(define x 42)` infers as `int` instead of `symbol`. Should be fixed in a future phase by tagging constructors or using a separate namespace. Does not block current functionality since `quote` is primarily used for ADT constructors in practice.

## Phase 3: Module System + Annotation Validation (NEXT)
- Type annotation checking
- Module import/export type checking