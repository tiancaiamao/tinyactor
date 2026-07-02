# Task A7: Add Function Name to Type Error Messages

## Problem
Type error messages don't include which function the error is in. Current format: `cannot unify int with string`. Desired: `in function 'foo': cannot unify int with string`.

## Current Architecture
- `unify_check(t1, t2, s)` in typecheck.ta records errors as `(tc_error . (t1 . (t2 . nil)))` in the substitution list
- `report_errors` / `print_error_list` iterates errors and prints them
- `unify_check` is called from:
  - 6 sites inside `infer_expr` (lines 654, 666, 804, 818, 880, 886)
  - 1 site inside `infer_define` (line 1206)
- `infer_define` knows the function name; calls `infer_lambda` → `infer_body` → `infer_expr`

## What to Implement

### Step 1: Change unify_check to accept ctx parameter
Change signature from `unify_check(t1, t2, s)` to `unify_check(t1, t2, s, ctx)` where `ctx` is a symbol (function name) or nil.

Change error record format from:
```
(tc_error . (t1 . (t2 . nil)))
```
To:
```
(tc_error . (t1 . (t2 . (ctx . nil))))
```

### Step 2: Thread ctx through inference functions
Add a `ctx` parameter to these functions (all in typecheck.ta):
- `infer_expr(expr, env, s, counter, level, ctx)` — line 578
- `infer_body(body, env, s, counter, level, ctx)` — line 781
- `infer_lambda(params, body, env, s, counter, level, ctx)` — line 744

All recursive calls within these functions pass `ctx` along unchanged.

### Step 3: Pass function name from infer_define
In `infer_define` (line 1190), the function name is available as `name`. Pass it as `ctx` to `infer_lambda`:
```
infer_lambda(params, body, env, s, counter, level + 1, name)
```
Also update the `unify_check` call on line 1206 to pass `name`:
```
unify_check(inferred_t, registered_t, s1, name)
```

### Step 4: Update all 6 unify_check call sites inside infer_expr
Change each from `unify_check(t1, t2, s)` to `unify_check(t1, t2, s, ctx)`.

### Step 5: Update collect_errors and print_error_list
- `collect_errors` — no change needed (it just finds tc_error markers)
- `print_error_list` — extract ctx from error record and include in message:
  - If ctx is not nil: `"  in function 'NAME': cannot unify T1 with T2"`
  - If ctx is nil: `"  cannot unify T1 with T2"` (backward compatible)

### Step 6: Update any test calls
Check the self-test functions at the bottom of typecheck.ta that call `infer_expr` or `infer_lambda` directly. Add `nil` as the ctx parameter.

## Files to Modify
- `lib/typecheck.ta` — all changes in this file

## Acceptance Criteria
1. Error messages include function name when available
2. Errors from anonymous lambdas (no name) still work (show no function name)
3. All existing typecheck tests still pass
4. Bootstrap works (`make && make bootstrap`)

## Verify Commands
```bash
cd /Users/genius/project/tinyactor

# 1. Error message includes function name
cat > /tmp/test-err-name.ta << 'EOF'
fn bad_math(x) {
  x + "hello"
}
fn main() {
  print("PASS")
}
EOF
NWORKERS=1 ./tinyactor --bootstrap /tmp/test-err-name.ta '' --check 2>&1
# Should output something like: "in function 'bad_math': cannot unify ..."

# 2. Existing tests still pass
bash test/run_all_tests.sh 2>&1 | tail -5
```

## Key Context
- `unify_check` is at line 313 of typecheck.ta
- Error format: `(tc_error . (t1 . (t2 . nil)))`
- `collect_errors` is around line 1505
- `report_errors` is at line 1520
- `print_error_list` is around line 1530
- `infer_define` is at line 1190, has `name` variable
- Self-tests are at the bottom (~line 1570+) and call infer_expr/infer_lambda directly