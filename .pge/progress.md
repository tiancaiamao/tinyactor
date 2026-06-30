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

## Phase 2: ADT + Pattern Match (NEXT)
- `type` declarations: `(type Name (quote V1) ...)`
- Constructor functions
- Exhaustiveness checking
- Pattern match type inference (already partially handled via parser desugar to let+if)

## Phase 3: Module System + Annotation Validation
- Type annotation checking
- Module import/export type checking