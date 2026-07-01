# Task: Implement ADT Type Declaration Support

## File to modify
`lib/typecheck.ta` (ONLY this file)

## Objective
Add `collect_type_decls` function and wire it into `infer_program`. Add `t_base` helper. Fix `type_format_resolved` for ADT type names. Modify `quote` handling to resolve nullary constructors. Add Tests G, H, I.

## Context

### Current `infer_program` (line ~898):
```ta
fn infer_program(forms) {
  let counter = 0
  let s = nil
  let be = make_builtin_env(counter)
  let env0 = car(be)
  let c0 = cdr(be)
  // Pass 1: collect all defines
  let cd = collect_defines(forms, env0, s, c0)
  let env1 = car(cd)
  let c1 = cdr(cd)
  // Pass 2: infer each define
  infer_defines(forms, env1, s, c1)
}
```

**Change needed:** Insert `collect_type_decls` call between builtin env and `collect_defines`:
```ta
  // Pass 0: collect type declarations (ADT constructors)
  let ctd = collect_type_decls(forms, env0, c0)
  let env_types = car(ctd)
  let c_types = cdr(ctd)
  // Pass 1: collect all defines
  let cd = collect_defines(forms, env_types, s, c_types)
```

### Current `quote` handling in `infer_compound` (line ~455):
```ta
  if head == 'quote {
    cons(t_symbol(), cons(s, counter))
  }
```

**Change needed:** Check if quoted symbol is a registered constructor:
```ta
  if head == 'quote {
    let quoted = car(rest)
    if symbol?(quoted) {
      let binding = assq(quoted, env)
      if null?(binding) == false {
        let inst = instantiate(cdr(binding), counter)
        cons(car(inst), cons(s, cdr(inst)))
      } else {
        cons(t_symbol(), cons(s, counter))
      }
    } else {
      cons(t_symbol(), cons(s, counter))
    }
  }
```

### Current `type_format_resolved` (line ~1008):
Has this chain for base types:
```ta
if name == 'pid { "pid" }
else { "unknown" }
```

**Change needed:** Replace `"unknown"` with `str.sym_to_str(name)` to render ADT type names like `Color`, `Option`, etc.

### Type representation:
- Base type: `(base name)` — e.g. `(base Color)`, `(base int)`
- Type variable: `(tvar id)` — e.g. `(tvar 0)`
- Arrow: `(arrow from to)` — e.g. `(arrow (tvar 0) (base Option))`
- Forall: `(forall (id_list) type)` — e.g. `(forall (0 1) (arrow (tvar 0) (arrow (tvar 1) (base Pair))))`

### Key existing functions you can use:
- `fresh(counter)` → `(type . counter)` — creates fresh tvar
- `extend(env, name, scheme)` → new env with binding
- `assq(key, alist)` → find binding or `nil`
- `instantiate(scheme, counter)` → `(type . counter)` — instantiates forall
- `t_forall(ids, t)` → forall type
- `t_arrow(from, to)` → arrow type
- `t_var(id)` → tvar type
- `list_length(lst)` → integer
- `str.sym_to_str(sym)` → converts symbol to string (C builtin, no import needed)
- `symbol?(x)` → true if x is a symbol

### Variant format from parser:
- Nullary: `(quote VariantName)` → car is `'quote`, cadr is the variant symbol
- N-ary: `(CtorName field1 field2 ...)` → car is the constructor name symbol, cdr is list of field names

## What to implement (in order):

1. Add `t_base(name)` helper near the other type constructors (around line 100)

2. Add `collect_type_decls(forms, env, counter)` → `(env . counter)`:
   - Walk forms recursively
   - For `(type Name v1 v2 ...)`:
     - For each variant, determine if nullary or n-ary
     - Nullary `(quote V)`: `extend(env, V, t_base(Name))`
     - N-ary `(Ctor f1 f2 ...)`: create fresh tvars for each field, build arrow chain ending in `t_base(Name)`, wrap in forall
   - Return `(env . counter)`

3. Add helper to build arrow chain for constructors if needed (keep it small, <30 statements)

4. Modify `infer_program` to call `collect_type_decls` before `collect_defines`

5. Modify `quote` case in `infer_compound` to check env for nullary constructors

6. Fix `type_format_resolved` to use `str.sym_to_str(name)` for unknown base types

7. Add test functions `test_g()`, `test_h()`, `test_i()` — each as its own function to stay under the 64-item limit

8. Add calls to `test_g()`, `test_h()`, `test_i()` at the end of `main()`, after the existing `print("=== ALL ACTOR TESTS DONE ===")` line

## Verify command
```bash
./tinyactor lib/typecheck.ta
```
Must produce output including Test G, H, I results and exit 0.