# Spec: Phase 2 — ADT Type Declarations in typecheck.ta

## Goal
Add support for `(type Name variant1 variant2 ...)` declarations so the type checker registers ADT constructors in the environment and correctly infers types for programs using them.

## Parser AST Format for Type Declarations

The parser outputs type declarations as:
```
(type TypeName variant1 variant2 variant3 ...)
```

Variants come in two forms:
- **Nullary** (no fields): `(quote VariantName)` — e.g. `(quote Red)`, `(quote None)`
- **N-ary** (with fields): `(VariantName field1 field2 ...)` — e.g. `(Some x)`, `(Cons head tail)`

The field names in N-ary variants are just placeholders (symbols like `x`, `head`). They indicate arity, not types. For now, all field types are fresh type variables (polymorphic).

## What to Implement

### 1. `collect_type_decls(forms, env, counter)` → `(env . counter)`

Walks top-level forms. For each `(type Name variant1 variant2 ...)`:

**Nullary variant** `(quote V1)`:
- Register `V1` in env with type `(base Name)` — a monomorphic base type.
- No forall needed — it's a simple value of type Name.

**N-ary variant** `(CtorName field1 field2 ...)`:
- Arity = number of fields.
- Constructor type: `field1_t -> field2_t -> ... -> (base Name)` where each field_t is a distinct fresh tvar.
- Wrap in forall: `forall(tvar_ids, arrow_chain)`.

Returns updated env and counter.

### 2. Modify `infer_program`

Call `collect_type_decls` **before** `collect_defines`, so constructor names are in scope when define bodies are inferred.

### 3. Modify `quote` handling in `infer_compound`

Currently `(quote x)` always returns `(base symbol)`. Change it to:
- If the quoted symbol is found in env (as a registered nullary constructor), return its type.
- Otherwise, fall back to `(base symbol)`.

### 4. Fix `type_format_resolved` for unknown base types

Currently renders any unrecognized base type name as `"unknown"`. Change to use `str.sym_to_str(name)` to render the actual symbol name (e.g. `"Color"`, `"Option"`).

### 5. Add helper `t_base(name)`

```ta
fn t_base(name) { cons('base, cons(name, nil)) }
```

## Implementation Constraints

- **ONLY modify `lib/typecheck.ta`**
- **NO `||` operator** (silently broken in .ta)
- **NO `!` operator** (use `expr == false`)
- Functions must be top-level only
- Use `match` for pattern matching
- **Keep functions small** — each function body should be under 30 statements to avoid the parser's `items[64]` limit
- **DO NOT modify `main()` function body** beyond adding test calls at the end — the existing test functions must not be touched
- **Build and run MUST work**: `./tinyactor lib/typecheck.ta` must produce output and exit 0

## Test Programs to Add

### Test G: Nullary ADT constructors
```ta
// type Color { Red; Green; Blue }
// fn main() { Red }
// Expected: main : Color
let prog = cons(
  cons('type, cons('Color,
    cons(cons('quote, cons('Red, nil)),
      cons(cons('quote, cons('Green, nil)),
        cons(cons('quote, cons('Blue, nil)), nil))))),
  cons(
    cons('define, cons(cons('main, nil),
      cons(cons('quote, cons('Red, nil)), nil))),
    nil))
// → main should have type "Color"
```

### Test H: N-ary ADT constructor
```ta
// type Option { Some(x); None }
// fn wrap(n) { Some(n) }
// fn main() { None }
// Expected: wrap : 'a -> Option, main : Option
let prog = cons(
  cons('type, cons('Option,
    cons(cons('Some, cons('x, nil)),
      cons(cons('quote, cons('None, nil)), nil)))),
  cons(
    cons('define, cons(cons('wrap, cons('n, nil)),
      cons(cons('Some, cons('n, nil)), nil))),
    cons(
      cons('define, cons(cons('main, nil),
        cons(cons('quote, cons('None, nil)), nil))),
      nil)))
// → wrap should have type "'a -> Option", main should have type "Option"
```

### Test I: ADT with 2-field constructor
```ta
// type Pair { MkPair(a b) }
// fn make(x) { MkPair(x, 42) }
// Expected: make : 'a -> Pair  (since MkPair takes two args, partially applied)
let prog = cons(
  cons('type, cons('Pair,
    cons(cons('MkPair, cons('a, cons('b, nil))), nil))),
  cons(
    cons('define, cons(cons('make, cons('x, nil)),
      cons(cons('MkPair, cons('x, cons(42, nil))), nil))),
    nil))
// → make should have type "'a -> Pair" or "int -> Pair" depending on unification
```

## Acceptance Criteria

### L1 — Structural
- [ ] `./tinyactor lib/typecheck.ta` runs and exits 0
- [ ] New functions exist: `collect_type_decls`, `t_base` — Verify: `grep -c 'fn collect_type_decls\|fn t_base' lib/typecheck.ta` ≥ 2
- [ ] `type_format_resolved` uses `str.sym_to_str` for unknown base types

### L2 — Behavioral
- [ ] Test G: `main` inferred as `Color`
- [ ] Test H: `wrap` inferred as `'a -> Option` (polymorphic constructor application)
- [ ] Test H: `main` inferred as `Option`
- [ ] Test I: `make` inferred without crash (some type involving Pair)
- [ ] All existing tests (Tests 1-7, A-F) still pass — output unchanged