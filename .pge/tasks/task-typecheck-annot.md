# Task: Type Checker — Annotation Validation via type-sig Forms

## File to modify
`lib/typecheck.ta` (ONLY this file)

## Objective
The parser now emits `(type-sig name (param_types...) ret_type)` forms alongside `(define (name params...) body)` forms when type annotations are present. The type checker needs to:

1. **Skip `type-sig` forms** in `collect_defines` and `infer_defines` (same as `import`, `type`)
2. **Use `type-sig` annotations for validation** — when inferring a define, if a matching type-sig exists, unify the inferred type with the annotated type
3. **Add test functions** J, K that demonstrate annotation validation

## Current State

### How type-sig forms look in the AST
The parser outputs programs as a list of top-level forms. For annotated code:
```
fn add(x: int, y: int) -> int { x + y }
```
The AST contains:
```
(type-sig add (int int) int)
(define (add x y) (+ x y))
```

For unannotated code:
```
fn id(x) { x }
```
The AST contains ONLY:
```
(define (id x) x)
```

### How the type checker processes top-level forms

`infer_program(forms)` (line ~993):
1. `make_builtin_env` → env0
2. `collect_type_decls` → env_types (ADT constructors)
3. `collect_defines` → env1 (registers each define name with fresh tvar)
4. `infer_defines` → infers each define body, unifies with registered type

Both `collect_defines` and `infer_defines` already skip non-define forms (`import`, `type`, etc.) via the else branch. `type-sig` will also be skipped by the else branch naturally.

### Key existing functions

- `t_int()` → `(base int)`, `t_string()` → `(base string)`, `t_bool()` → `(base bool)`, `t_pid()` → `(base pid)`
- `t_base(name)` → `(base name)` — for custom ADT type names
- `t_var(id)` → `(tvar id)` — type variable
- `t_arrow(from, to)` → `(arrow from to)` — function type
- `t_forall(ids, type)` → `(forall (ids) type)` — quantified type
- `fresh(counter)` → `(type . counter)` — creates fresh tvar
- `unify(t1, t2, s)` → `((succ/fail) . substitution)` — unifies two types
- `assq(key, alist)` → finds binding or nil
- `extend(env, name, scheme)` → adds binding to env
- `apply_subst(t, s)` → resolves type through substitution
- `instantiate(scheme, counter)` → `(type . counter)` — instantiates forall

## What to Implement

### 1. `parse_type_annot(sym)` → type or nil

Converts an annotation symbol to internal type representation:
```ta
fn parse_type_annot(sym) {
  if null?(sym) { nil }
  else {
    if sym == 'int { t_int() }
    else {
      if sym == 'string { t_string() }
      else {
        if sym == 'bool { t_bool() }
        else {
          if sym == 'pid { t_pid() }
          else {
            if sym == 'Pid { t_pid() }
            else { t_base(sym) }
          }
        }
      }
    }
  }
}
```

### 2. `annot_to_type(param_types, ret_type, counter)` → (type . counter)

Builds an arrow type from annotation lists. For each param type:
- If annotated: use `parse_type_annot`
- If nil (unannotated): use fresh tvar

Return type:
- If annotated: use `parse_type_annot`
- If nil: use fresh tvar

Build curried arrow: `param1_t -> param2_t -> ... -> ret_t`

The type may contain free tvars (from unannotated params/ret), so it should NOT be wrapped in forall — leave it monomorphic. The unify will handle it.

### 3. `collect_type_sigs(forms, sig_env, counter)` → (sig_env . counter)

Walks top-level forms looking for `(type-sig name param_types ret_type)`. For each:
- Call `annot_to_type` to build the expected type
- Store in a separate assoc list: `(name . expected_type)` pairs
- Return the assoc list (not the main env — this is a SEPARATE structure for annotations)

### 4. Modify `infer_define` to check annotations

In `infer_define(form, env, s, counter)`, after inferring the lambda type:
- Check if there's a matching type-sig for this function name
- If yes, unify the inferred type with the annotated type
- Be permissive: if unification fails, silently continue (don't report errors yet)

The sig_env needs to be accessible from infer_define. The cleanest approach:
- Add sig_env as a parameter to infer_define, OR
- Store sig_env globally (but TA has no globals), OR
- Merge annotations into the main env

**Simplest approach:** Modify `infer_program` to:
1. Collect type-sigs into an assoc list
2. Pass the assoc list to `infer_defines`, which passes it to `infer_define`

This changes the signature of `infer_defines` and `infer_define` to accept an extra `sigs` parameter. Each function just needs an extra argument threaded through.

Actually, even simpler: **pre-register annotated types in collect_defines**. When collect_defines sees a define, it checks if there's a matching type-sig. If so, instead of registering a fresh tvar, it registers the annotated type. Then infer_define unifies inferred with registered as usual.

This means:
- `collect_type_sigs` runs BEFORE `collect_defines`
- `collect_defines` receives the sig list
- When registering a define name: if it's in sig list, use the annotated type instead of fresh tvar

### 5. Add `type-sig` to skip list in collect_defines and infer_defines

Both already skip non-define forms. Verify `type-sig` is skipped (it should be, since head != 'define).

### 6. Test J: Annotated function

```ta
fn test_j() {
  print("=== Test J: annotated function ===")
  // Source: fn add(x: int, y: int) -> int { x + y }
  // AST: (type-sig add (int int) int) (define (add x y) (+ x y))
  let prog = cons(
    cons('type-sig, cons('add, cons(cons('int, cons('int, nil)), cons('int, nil)))),
    cons(
      cons('define, cons(cons('add, cons('x, cons('y, nil))),
        cons(cons('+, cons('x, cons('y, nil))), nil))),
      nil))
  let ip = infer_program(prog)
  let env = car(ip)
  let s = car(cdr(ip))
  let b = assq('add, env)
  let sch = cdr(b)
  print(type_format(sch, s))
}
// Expected output: "int -> int -> int" (or "int -> (int -> int)" depending on format)
```

### 7. Test K: Partially annotated function

```ta
fn test_k() {
  print("=== Test K: partial annotation ===")
  // Source: fn wrap(x: int) { x }   (no return annotation)
  // AST: (type-sig wrap (int) nil) (define (wrap x) x)
  let prog = cons(
    cons('type-sig, cons('wrap, cons(cons('int, nil), cons(nil, nil)))),
    cons(
      cons('define, cons(cons('wrap, cons('x, nil)),
        cons('x, nil))),
      nil))
  let ip = infer_program(prog)
  let env = car(ip)
  let s = car(cdr(ip))
  let b = assq('wrap, env)
  let sch = cdr(b)
  print(type_format(sch, s))
}
// Expected: "int -> int" (param annotated as int, return inferred as int from body)
```

### 8. Add test calls to main()

Add `test_j()` and `test_k()` calls at the end of `main()`, after the existing tests.

## Implementation Strategy

The cleanest approach with minimal signature changes:

1. Add `parse_type_annot` and `annot_to_type` helpers
2. Add `collect_type_sigs` that scans for type-sig forms and returns assoc list
3. Modify `collect_defines` to accept a `sigs` parameter. When registering a define:
   - Look up name in sigs
   - If found, use the annotated type (from annot_to_type)
   - If not found, use fresh tvar (as before)
4. Modify `infer_program` to call collect_type_sigs, then pass sigs to collect_defines
5. Add tests J and K

## Constraints
- ONLY modify `lib/typecheck.ta`
- NO `||` or `!` operators
- Functions top-level only
- Keep functions under 30 statements
- All existing tests (1-7, A-I) must still pass

## Verify
```bash
./tinyactor lib/typecheck.ta
```
Must show Test J and K results, all existing tests unchanged, and exit 0.