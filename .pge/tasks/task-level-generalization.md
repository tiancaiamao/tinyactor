# Task: Level-Based Generalization Optimization for typecheck.ta

## Goal
Eliminate the O(N³) bottleneck in `generalize()` by replacing `free_vars_env()` with level-based generalization. Expected: parser.ta from ~70s to ~5s.

## Background
The current `generalize(t, env, s)` calls `free_vars_env(env, s)` which scans the ENTIRE environment at every `define`/`let`. At step k, env has k bindings, each `free_vars_scheme` calls `apply_subst` (O(|S|) ≈ O(k)). Total: O(N³).

Level-based generalization eliminates this by tracking the "binding level" of each tvar. Instead of scanning the env, we just check which tvars have level == current level. Those are safe to generalize.

## File
Only modify: `/Users/genius/project/tinyactor/lib/typecheck.ta`

## Changes Required

### 1. Change tvar representation
Old: `('tvar id)` = `cons('tvar, cons(id, nil))`
New: `('tvar id level)` = `cons('tvar, cons(id, cons(level, nil)))`

- `car(cdr(t))` still gets id — ALL existing code keeps working
- `car(cdr(cdr(t)))` gets level (new accessor)

Update `t_var(id)` to `t_var(id, level)` — takes 2 args now.

### 2. Update `fresh(counter)` → `fresh(counter, level)`
```ta
fn fresh(counter, level) {
  cons(t_var(counter, level), counter + 1)
}
```

### 3. Thread `level` through inference functions
Add `level` parameter to these functions (pass it through to all sub-calls):

- `infer_expr(expr, env, s, counter)` → `infer_expr(expr, env, s, counter, level)`
- `infer_compound(expr, env, s, counter)` → add `level`
- `infer_lambda(params, body, env, s, counter)` → add `level` (lambda params use current level)
- `infer_body(body, env, s, counter)` → add `level`
- `infer_arith(args, env, s, counter)` → add `level`
- `infer_cmp(args, env, s, counter)` → add `level`
- `infer_eq(args, env, s, counter)` → add `level`
- `infer_call(head, args, env, s, counter)` → add `level`
- `infer_args_for_unknown(args, env, s, counter)` → add `level`
- `infer_apply(fn_t, args, env, s, counter)` → add `level`
- `infer_define(form, env, s, counter)` → add `level`
- `infer_defines(forms, env, s, counter)` → add `level`

ALL `fresh(counter)` calls inside these functions become `fresh(counter, level)` or `fresh(cN, level)`.

### 4. Update `let` case in `infer_compound`
The `let` case needs special level handling:
```ta
// OLD:
let rv = infer_expr(val_expr, env, s, counter)
let scheme = generalize(val_t, env, s1)

// NEW:
let rv = infer_expr(val_expr, env, s, counter, level + 1)
let scheme = generalize_l(val_t, level, s1)
let new_env = extend(env, var_sym, scheme)
infer_body(body, new_env, s1, c1, level + 1)
```

### 5. New `generalize_l` function (replaces `generalize`)
```ta
// generalize_l(t, level, s) -> scheme
// Generalize tvars whose level > current level
fn generalize_l(t, level, s) {
  let t_ftv = free_vars_t(t, s)
  let quantified = collect_level_gt(t_ftv, s, level)
  if null?(quantified) {
    t
  } else {
    t_forall(quantified, t)
  }
}

// collect_level_gt(tvar_ids, s, level) -> list of ids whose level > level
fn collect_level_gt(ids, s, level) {
  match ids {
    nil -> nil
    cons(id, rest) -> {
      let tv = apply_subst(t_var(id, 0), s)  // resolve to get actual type
      let actual_level = if car(tv) == 'tvar { car(cdr(cdr(tv))) }
                         else { 0 }  // non-tvar, already resolved
      if actual_level > level {
        cons(id, collect_level_gt(rest, s, level))
      } else {
        collect_level_gt(rest, s, level)
      }
    }
  }
}
```

WAIT — there's a subtlety. After apply_subst, a tvar might resolve to another tvar (with a different level). We need the level of the RESOLVED tvar, not the original. Actually, `free_vars_t` already calls `apply_subst` and returns the resolved tvar ids. So the ids returned by `free_vars_t` are already resolved.

But we need the levels of those resolved tvars. Since `free_vars_t` returns just ids (integers), we need to look up the level. The resolved tvar is in the substitution already.

Simpler approach: modify `free_vars_t` to return `(id . level)` pairs, OR create a new function `free_vars_t_with_level`.

ACTUALLY, the simplest correct approach:

```ta
fn generalize_l(t, level, s) {
  // Get resolved type
  let resolved = apply_subst(t, s)
  // Find tvars in resolved type whose level > current level
  let quantified = find_generalizable(resolved, level, s)
  if null?(quantified) {
    resolved
  } else {
    t_forall(quantified, resolved)
  }
}

// find_generalizable(t, level, s) -> list of tvar ids with level > level
fn find_generalizable(t, level, s) {
  let tag = car(t)
  if tag == 'tvar {
    let id = car(cdr(t))
    let tv_level = car(cdr(cdr(t)))
    if tv_level > level {
      cons(id, nil)
    } else {
      nil
    }
  } else {
    if tag == 'arrow {
      list_union(
        find_generalizable(car(cdr(t)), level, s),
        find_generalizable(car(cdr(cdr(t))), level, s)
      )
    } else {
      if tag == 'app {
        find_generalizable_list(cdr(cdr(t)), level, s)
      } else {
        nil
      }
    }
  }
}

fn find_generalizable_list(ts, level, s) {
  match ts {
    nil -> nil
    cons(h, rest) -> list_union(find_generalizable(h, level, s), find_generalizable_list(rest, level, s))
  }
}
```

This walks the TYPE (not the env), checking each tvar's level. Cost: O(T) where T is type size.

### 6. Update `infer_define`
```ta
// OLD:
let scheme = generalize(resolved_t, env, s2)

// NEW:
let scheme = generalize_l(resolved_t, level, s2)
```

### 7. Update `infer_program` and `infer_defines`
- `infer_program`: call `infer_defines` with level 0
- `infer_defines`: pass level through to `infer_define`

### 8. Update collect_defines and collect_type_decls
These functions call `fresh(counter)` during the collection phase. They should use level 0:
- `fresh(counter)` → `fresh(counter, 0)`

Also update `annot_to_type`, `annot_to_type_params`, `make_fresh_tvars` to pass level 0.

### 9. Update t_var calls in initial env and tests
All `t_var(0)`, `t_var(1)`, `t_var(2)` calls in:
- `make_builtin_env` (lines ~1224-1276)
- Test functions (lines ~1727+)

These are inside `forall` schemes (already quantified), so level 0 is correct:
- `t_var(0)` → `t_var(0, 0)`

### 10. Keep old functions for compatibility
Keep `free_vars_env`, `free_vars_scheme` as dead code (they won't be called anymore). Or remove them if you prefer.

## Validation
After implementing:
1. Run `NWORKERS=1 ./tinyactor --bootstrap lib/tokenizer.ta '' --check` — should still work (no errors)
2. Run `NWORKERS=1 ./tinyactor --bootstrap test/scripts/typecheck-errors.ta '' --check` — should find 2 errors
3. Time `NWORKERS=1 ./tinyactor --bootstrap lib/parser.ta '' --check` — should be MUCH faster than 70s
4. Run full test suite: `bash test/run_all_tests.sh`
5. Rebuild bootstrap: `make bootstrap`

## Important Notes
- The TA language uses `if/else` chains, not `cond` or pattern matching in most places
- Be careful with the `let` case: the value expression gets `level + 1`, but the body ALSO gets `level + 1` (it's a new scope)
- For `lambda`: parameters get fresh tvars at the CURRENT level (not level+1). The lambda body is inferred at the same level.
- For `define` at top level: everything at level 0
- `infer_program` starts at level 0
- Keep `free_vars_t` as-is (it's still used by `generalize_l` indirectly through `find_generalizable`)
- The old `generalize` function can be removed or kept as dead code