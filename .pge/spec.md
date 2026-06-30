# Spec: Implement infer_program pipeline in typecheck.ta

## Goal
Extend `lib/typecheck.ta` to type-check entire programs (list of `(define ...)` forms), not just individual expressions.

## Background

`lib/typecheck.ta` already has working HM type inference for expressions:
- `unify(x, y, s)` → `('succ . subst)` or `('fail . subst)`
- `apply_subst(t, s)` → resolved type
- `fresh(counter)` → `(type_var . new_counter)`
- `infer_expr(expr, env, s, counter)` → `(type . (s . counter))`
- `infer_lambda(params, body, env, s, counter)` → curried arrow type
- `generalize(t, env, s)` → `(forall (ids) t)` scheme
- `instantiate(scheme, counter)` → `(type . counter)`
- `t_int()`, `t_bool()`, `t_string()`, `t_var(id)`, `t_arrow(from, to)`, `t_forall(ids, t)`
- env = alist of `(name . scheme)`, s = alist of `(var_id . type)`

## .ta Language Quirks (CRITICAL — .ta is NOT Lisp)

1. **NO `||` operator** — it silently returns wrong values. Use nested `if/else`.
2. **NO `!` operator** — use `expr == false` instead.
3. **NO `and`/`or` keywords**.
4. `fn name(params...) { body }` — function syntax (not `define`)
5. `let x = expr in body` — let binding
6. `if cond { then } else { else }` — if expression
7. `cons(a, b)`, `car(p)`, `cdr(p)`, `null?(x)`, `pair?(x)`, `int?(x)`, `string?(x)`, `symbol?(x)`
8. `assq(key, alist)` — returns `(key . val)` or `nil`
9. `list_ref(lst, n)` — nth element (0-indexed)
10. `print(x)` — prints value
11. `'symbol` — quoted symbol literal
12. `nil` — empty list
13. Functions can ONLY be defined at top level (no local fn)
14. Curried arrow types: `fn(x,y){...}` → `t_arrow(tx, t_arrow(ty, tret))`

## What to Implement

### 1. `collect_defines(forms, env, s, counter)`
Pass 1: Walk `forms` (list of top-level forms). For each `(define (name params...) body...)` or `(define_pub ...)`:
- Create a fresh tvar for the function
- Add `(name . fresh_tvar)` to env
- Return `(env . counter)`

This enables mutual recursion (all function names in scope before any body is inferred).

### 2. `infer_define(form, env, s, counter)`
Process one `(define (name params...) body...)`:
- Extract name from `car(list_ref(form, 1))`
- Extract params from `cdr(list_ref(form, 1))`  
- Extract body from `cdr(cdr(form))`
- Use `infer_lambda(params, body, env, s, counter)` to get function type
- Look up pre-registered type in env, unify with inferred type
- Generalize the result type
- Update env binding to the generalized scheme
- Return `(env . (s . counter))`

### 3. `infer_program(forms)`
Top-level entry point:
- Initialize counter=0, s=nil
- Build initial env with builtin function types
- Pass 1: `collect_defines` to register all functions
- Pass 2: For each define, call `infer_define`, generalize immediately
- Pass 3: Check top-level non-define expressions
- Return final env (with all inferred types)

### 4. `type_format(t, s)`
Pretty-print a type after applying substitutions:
- `(base int)` → `"int"`
- `(base bool)` → `"bool"`  
- `(base string)` → `"string"`
- `(tvar id)` → `"'a"`, `"b"`, `"c"` etc. (assign letters by first-seen order)
- `(arrow A B)` → `"A -> B"`
- `(forall (ids) t)` → format inner `t` (ids are implicit in pretty output)

### 5. Builtins
Add these to the initial environment in `infer_program`:
- `car`: `forall(a, arrow(a, a))` — actually `forall(a, arrow(list_a, a))` but we don't have list types, so just `forall(a, arrow(a, a))` (permissive)
- `cdr`: same permissive approach
- `cons`: `forall(a, forall(b, arrow(a, arrow(b, b))))` — permissive
- `print`: `forall(a, arrow(a, nil))` — but nil type is a fresh var, so `forall(a, arrow(a, b))`
- `len`: `forall(a, arrow(a, int))`
- `str_concat`: `arrow(string, arrow(string, string))`
- `pair?`, `null?`, `int?`, `string?`, `symbol?`: `forall(a, arrow(a, bool))`
- `list_ref`: `forall(a, arrow(a, arrow(int, a)))` — permissive

These can be permissive (using fresh vars) since we prioritize not crashing over precision.

### 6. Test Cases (in main)
Replace/augment existing tests with:
```
// Define a simple function
let prog1 = cons(cons('define,
  cons(cons('double, cons('x, nil)),
    cons(cons('*, cons('x, cons(2, nil))), nil))), nil)
// → double: int -> int

// Define with multiple params  
let prog2 = cons(cons('define,
  cons(cons('add, cons('x, cons('y, nil))),
    cons(cons('+, cons('x, cons('y, nil))), nil))), nil)
// → add: int -> int -> int

// Recursive function (factorial)
let prog3 = list(
  cons('define,
    cons(cons('fact, cons('n, nil)),
      cons(cons('if,
        cons(cons('<=, cons('n, cons(1, nil))),
        cons(1,
        cons(cons('*, cons('n, cons(cons('fact, cons(cons('-', cons('n, cons(1, nil))), nil)), nil))), nil)))),
      nil))))
// → fact: int -> int

// Mutual recursion: even/odd
let prog4 = list(
  define('is_even', [n], if ... ),
  define('is_odd', [n], if ...))

// Higher-order: map
let prog5 = cons(cons('define,
  cons(cons('map, cons('f, cons('lst, nil))),
    cons(cons('if,
      cons(cons('null?, cons('lst, nil)),
      cons(nil,
      cons(cons('cons,
        cons(cons('f, cons(car(lst), nil)),
        cons(cons('map, cons('f, cons(cdr(lst), nil))), nil))),
      nil)))),
    nil))), nil)
// → map: (a -> b) -> list(a) -> list(b)  (but permissively, tvars)
```

## Acceptance Criteria

### L1 — Structural
- [ ] `./tinyactor lib/typecheck.ta` runs without crash and exits cleanly — Verify: `./tinyactor lib/typecheck.ta 2>&1 | tail -5`
- [ ] All new functions exist: collect_defines, infer_define, infer_program, type_format — Verify: `grep -c 'fn collect_defines\|fn infer_define\|fn infer_program\|fn type_format' lib/typecheck.ta` returns 4
- [ ] `make -j4` succeeds — Verify: `make -j4 2>&1 | tail -1`

### L2 — Behavioral
- [ ] `infer_define` on `(define (double x) (* x 2))` produces `int -> int` — Verify: run `./tinyactor lib/typecheck.ta` and check test output contains expected types
- [ ] `infer_define` on `(define (add x y) (+ x y))` produces `int -> int -> int`
- [ ] Recursive function (factorial) infers as `int -> int`
- [ ] Higher-order function (map-like) infers with polymorphic arrow type
- [ ] `infer_program` on a multi-function program infers all types correctly
- [ ] `type_format` produces readable output like "int", "int -> int", "'a -> 'a"

## Constraints
- ONLY modify `lib/typecheck.ta`
- No new VM opcodes
- No mutable state (pure functional style with threading)
- Follow existing code style in typecheck.ta
- The `||` operator does NOT work in .ta — use nested if/else
- Functions are top-level only (no local fn definitions)

## Out of Scope
- Integration with compile.c pipeline (separate task)
- C typecheck.c changes
- Error message formatting beyond type_format
- ADT/type declaration support