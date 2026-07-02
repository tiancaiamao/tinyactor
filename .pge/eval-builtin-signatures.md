# Eval Report: A8 — Builtin Function Type Signatures

## Verdict: PASS

## Criteria Results
1. [✅] Bootstrap succeeds
2. [✅] Self-test runs
3. [✅] Error test no regression
4. [✅] make test baseline maintained
5. [✅] Type signatures correct
6. [✅] No DEBUG prints
7. [✅] Only typecheck.ta modified

## Evidence

### Criterion 1: Bootstrap succeeds
```
$ make bootstrap
./tinyactor lib/driver.ta --emit-tabc
wrote lib/driver.tabc
cp lib/driver.tabc lib/bootstrap.tabc
wrote lib/bootstrap.tabc
```
No errors.

### Criterion 2: Self-test runs
```
$ NWORKERS=1 ./tinyactor --bootstrap lib/typecheck.ta '' --check
=== ALL BASIC TESTS DONE ===
=== ALL ACTOR TESTS DONE ===
```
Both markers present.

### Criterion 3: Error test no regression
```
$ NWORKERS=1 ./tinyactor --bootstrap test/scripts/typecheck-errors.ta '' --check
typecheck: 2 type error(s) found
  in function 'bad_if':   cannot unify int with bool
  in function 'bad_call':   cannot unify string with 'a
PASS
```
2 type errors with function names, then PASS — matches expected behavior exactly.

### Criterion 4: make test baseline
Multiple runs show 183–187 passed out of 188. The only failing tests are flaky concurrency actor tests (echo_test, error-send-to-dead, monitor_test) — consistent with the pre-existing 186/188 baseline. All typecheck-related tests pass consistently:
```
typecheck-clean.ta:         ✅ PASS
typecheck-errors.ta:        ✅ PASS
bytecode-cmp typecheck-*:   ✅ PASS
```

### Criterion 5: Type signatures correct
Verified against source (lines 1335–1380):

| Function | Expected | Actual | ✓ |
|----------|----------|--------|---|
| str.char_at | string -> int -> int | `t_arrow(t_string(), t_arrow(t_int(), t_int()))` | ✅ |
| str.substr | string -> int -> int -> string | `t_arrow(t_string(), t_arrow(t_int(), t_arrow(t_int(), t_string())))` | ✅ |
| str.to_int | string -> int | `t_arrow(t_string(), t_int())` | ✅ |
| str.from_int | int -> string | `t_arrow(t_int(), t_string())` | ✅ |
| str.index_of | string -> string -> int | `t_arrow(t_string(), t_arrow(t_string(), t_int()))` | ✅ |
| str.to_sym | forall(a, string -> a) | `t_forall(cons(0,nil), t_arrow(t_string(), t_var(0,0)))` | ✅ |
| str.sym_to_str | forall(a, a -> string) | `t_forall(cons(0,nil), t_arrow(t_var(0,0), t_string()))` | ✅ |
| +,-,*,/ | int -> int -> int | `t_arrow(t_int(), t_arrow(t_int(), t_int()))` (shared arith_t) | ✅ |
| <,>,<=,>= | int -> int -> bool | `t_arrow(t_int(), t_arrow(t_int(), t_bool()))` (shared cmp_t) | ✅ |
| == | forall(a, a -> a -> bool) | `t_forall(cons(0,nil), t_arrow(t_var(0,0), t_arrow(t_var(0,0), t_bool())))` | ✅ |
| not | bool -> bool | `t_arrow(t_bool(), t_bool())` | ✅ |

### Criterion 6: No DEBUG prints
`grep DEBUG lib/typecheck.ta` → no matches.

### Criterion 7: Only typecheck.ta modified
`git diff --name-only` → only `lib/typecheck.ta`. No C files touched.

## Notes
- Minor cosmetic: one existing comment (`// str.length: string -> int`) got extra indentation in the diff due to line repositioning. Functionally harmless.
- The `list` builtin was intentionally skipped per the task notes (variadic, tricky to type precisely). This is acceptable.
- Chaining follows the existing pattern cleanly (e23 → e24 ... → e40 → `cons(e40, counter)`).