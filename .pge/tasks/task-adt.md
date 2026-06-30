# Task: Add ADT (Algebraic Data Type) support to typecheck.ta

## Context

`lib/typecheck.ta` is a Hindley-Milner type checker for TinyActor. It currently handles:
- Basic types (int, bool, string, symbol, pid)
- Function types (arrow), type variables (tvar), polymorphic schemes (forall)
- Actor primitives (spawn, send, recv, self, monitor, receive-scan)
- Pattern matching (already desugared by parser into let+if chains)

Phase 2 adds support for `type` declarations and ADT constructors.

## Parser AST Format for Type Declarations

The parser produces these forms for `type` declarations:

```
type Color { Red, Green, Blue }
→ (type Color (quote Red) (quote Green) (quote Blue))

type Option { Some(x), None }
→ (type Option (Some x) (quote None))

type Pair { Pair(a, b) }
→ (type Pair (Pair a b))
```

Key observations:
- Nullary variants: `(quote Name)` — these are just values of the type
- N-ary variants: `(CtorName field1 field2 ...)` — constructor functions
- Variant field names (like `x`, `a`, `b`) are just placeholder symbols; we only care about arity

At runtime:
- `Some(42)` is a function call `(Some 42)` → creates `(Some . (42 . nil))` (pair)
- Nullary `Red` → `(quote Red)` (quoted symbol)
- Pattern matching on constructors is already desugared to `pair?`, `car`, `cdr`, `==`, `str.eq` chains

## What to Implement

### 1. Add `collect_type_decls(forms, env, s, counter) -> (env . counter)`

Process `(type Name variant1 variant2 ...)` forms:
- For each `type` form, extract the type name (symbol) and iterate variants
- **Nullary variant** `(quote VName)`: register symbol `VName` in env with scheme:
  `forall((), (base TypeName))` — i.e. just the base type, no quantified vars
  Implementation: `t_forall(nil, t_base(TypeName))` (empty forall list = monomorphic)
- **N-ary variant** `(CtorName f1 f2 ... fN)`: register symbol `CtorName` with scheme:
  `forall(a1...aN, arrow(a1, arrow(a2, ... arrow(aN, (base TypeName)))))`
  Implementation: create `N` fresh-type-var IDs (0..N-1), build nested arrows, wrap in forall
- Return updated env and counter (unchanged — constructor schemes use relative forall IDs like builtins)

### 2. Call `collect_type_decls` in `infer_program`

In `infer_program`, after `make_builtin_env` and before `collect_defines`, call:
```
let ctd = collect_type_decls(forms, env0, s, c0)
let env_types = car(ctd)
let c_types = cdr(ctd)  // actually same as c0 since we use relative forall IDs
```
Then pass `env_types` to `collect_defines`.

### 3. Add ADT name support in `type_format_resolved`

Currently `type_format_resolved` handles `(base name)` with hardcoded checks for int/string/bool/symbol/pid.
Add a fallback: if name is none of the known base types, use `str.sym_to_str(name)` to get the string representation.

### 4. Add `t_base(name)` helper (if not already existing)

A helper that creates `(base name)` — check if it already exists. Currently `t_int`, `t_bool`, etc. are separate. A generic `t_base(name)` that takes a symbol would be useful.

## Important Constraints

- **NO `||` operator** — it's silently broken in .ta
- **NO `!` operator** — use `expr == false`
- Functions are top-level only
- Use `match` for pattern matching (supported in .ta)
- The parser has a 64-statement limit per function body — keep new functions under 64 statements
- `str.sym_to_str(symbol)` converts a symbol to a string (available without import)
- All existing tests (1-7, A-F) must still pass
- `str.concat` is available for string concatenation

## Test Cases to Add

### Test G: Type declaration with nullary constructors
```
type Color { Red, Green, Blue }
fn main() { Red }
```
AST: `(type Color (quote Red) (quote Green) (quote Blue)) (define (main) (quote Red))`
Expected: `main` has type involving `(base Color)` — since Red : (base Color)

### Test H: Type declaration with n-ary constructor
```
type Option { Some(x), None }
fn wrap(n) { Some(n) }
```
AST: `(type Option (Some x) (quote None)) (define (wrap n) (Some n))`
Expected: `wrap : 'a -> Option` (polymorphic, since Some takes any type)

### Test I: Type declaration + pattern match (desugared)
```
type Option { Some(x), None }
fn unwrap(opt, default) {
  match opt {
    Some(v) -> v
    _ -> default
  }
}
```
This desugars to: `(let temp opt (if (pair?(temp) && ==(car(temp), (quote Some))) (let v (car(cdr(temp))) v) default))`
Expected: `unwrap : 'a -> 'a -> 'a` (should work — pair?, car, cdr, == are already in builtin env)

### Test J: Full ADT program (actor + ADT)
```
type Msg { Ping(pid), Pong, Stop }
fn handler() {
  receive {
    Ping(from) -> send(from, Pong)
    Stop -> nil
    _ -> nil
  }
}
```
This desugars with receive-scan. Expected: type-checks without error.

## Acceptance Criteria

1. `./tinyactor lib/typecheck.ta` runs and exits 0
2. All existing tests (1-7, A-F) still produce identical output
3. Test G: nullary constructor `Red` resolves to type involving Color
4. Test H: `Some(n)` resolves — `wrap` has type `a -> Option`
5. Test I: pattern match on ADT type-checks
6. Test J: receive + ADT type-checks without crash
7. `./tinyactor --bootstrap lib/typecheck.ta` still works