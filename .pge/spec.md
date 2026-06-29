# Spec: Hindley-Milner Type System for TinyActor

## Goal
Add a compile-time type checker with HM type inference, type annotations, and exhaustive pattern matching — inspired by Gleam. The type checker runs between parsing and code generation. If type checking passes, bytecode output is unchanged (zero runtime overhead).

## Current State
- ADT with payload **already works at runtime** (`Ping(x)` → `(cons (quote Ping) (cons x nil))`)
- Type annotations are **parsed but discarded** by reader_ta.c
- Exhaustiveness checking exists as a **runtime stderr warning** (buggy)
- No type inference, no type error detection

## Design

### Runtime Representation (UNCHANGED)
Types are purely compile-time. Runtime values stay the same:
- `int` → TAG_INT, `bool` → TAG_TRUE/FALSE, `nil` → TAG_NIL
- `string` → TAG_STRING, `pid` → TAG_PID, `symbol` → TAG_SYM
- ADT variants → existing cons-of-quoted-symbol representation
- No new heap types, no VM changes, no GC changes

### Type Representation (NEW — in typecheck.h)
```c
typedef enum { TY_VAR, TY_CON, TY_ARROW, TY_ADT } TypeKind;
typedef struct Type Type;
struct Type {
    TypeKind kind;
    union {
        struct { int id; Type *instance; } var;      // TY_VAR: unification variable
        struct { const char *name; } con;              // TY_CON: int, bool, string, pid, nil
        struct { Type **params; Type *ret; int np; } arrow; // TY_ARROW
        struct { const char *name; Type **args; int nargs; } adt; // TY_ADT: Msg, Option, etc.
    };
};
```

### Type Annotations (MODIFIED reader_ta.c)
Currently skipped. Need to capture them:
- `fn(x: int, y: string) -> bool` → store param types + return type in AST
- `let x: int = expr` → store type annotation
- AST representation: attach type info as extra elements in the pair-tree

### Type Checker Pipeline (NEW — src/typecheck.c)
```
reader → AST → typecheck.c (type check + inference) → compile.c → bytecode
```
The type checker:
1. Collects type declarations (ADTs) and function signatures
2. Performs HM inference on every expression
3. Checks declared annotations against inferred types
4. Reports type errors with location info (function name, line if possible)
5. Checks match exhaustiveness as a compile error (not warning)

### Unification Algorithm
Standard Robinson unification:
- `unify(t1, t2)`: makes t1 and t2 equal, fails on mismatch
- `prune(t)`: follows type variable chain to representative
- `occurs_check`: prevent infinite types

### Generalization (Let-Polymorphism)
Functions defined at top level are generalized (made polymorphic) over free type variables.
Example: `fn id(x) { x }` gets type `∀a. a → a`.

## Acceptance Criteria

### L1 — Structural
- [ ] `src/typecheck.h` exists with Type representation
- [ ] `src/typecheck.c` exists and compiles
- [ ] `make` passes with typecheck.c added to Makefile
- [ ] reader_ta.c captures type annotations (param types, return types)
- [ ] Type checker is called in compile pipeline before code generation
- [ ] Verify: `make 2>&1 | grep -c error` returns 0

### L2 — Behavioral
- [ ] Type inference: `fn add(x, y) { x + y }` → inferred as `(int, int) → int`
- [ ] Type checking catches: `fn bad(x: int) { x + true }` → compile error
- [ ] ADT checking: `type Opt { Some(int); None }`, `Some(true)` → compile error
- [ ] Pattern match exhaustiveness: missing variant → compile error (not warning)
- [ ] Polymorphism: `fn id(x) { x }` works with both int and string
- [ ] All existing tests still pass (no behavioral regression)
- [ ] Type annotations are optional: code without annotations still works
- [ ] Verify: `make test` passes ≥176/177 (echo_test exception)
- [ ] Type error test: `./tinyactor test/scripts/type-error-test.ta` exits non-zero with error message
- [ ] Type pass test: `./tinyactor test/scripts/type-ok-test.ta` runs normally

## Constraints
- Type checking is a compile-time pass — ZERO runtime overhead
- Do NOT modify gc.c (GC files cannot be modified)
- Do NOT modify VM bytecode or value representation
- reader_ta.c changes must be backward compatible (old .ta files still compile)
- Type annotations remain OPTIONAL — unannotated code works via inference
- Type errors should print to stderr with useful messages
- Type errors do NOT halt compilation by default (warnings mode), unless a flag is set — BUT exhaustiveness errors are hard errors
- Actually: type mismatches should be ERRORS that prevent running. This is the whole point. But existing tests must still pass, which means existing code must be type-correct.

## Out of Scope
- Type classes / traits / interfaces (Gleam doesn't have these either)
- Row polymorphism / extensible records
- Type aliases (can be added later)
- Effect types
- Actor message type checking (future enhancement)

## Implementation Phases

### Phase 1: Foundation (typecheck.h + typecheck.c skeleton)
- Type representation (Type struct, constructors, free/clone)
- Type environment (name → Type mapping)
- Unification engine (unify, prune, occurs_check, generalize, instantiate)
- Basic type inference for: literals, let, if, binary ops, function calls
- Wire into compile pipeline (call before codegen)
- All existing tests must still pass

### Phase 2: ADT + Pattern Matching Types
- Type declaration parsing → register ADT types and constructors
- Constructor type checking (Some(42) : Option<int>)
- Pattern match exhaustiveness as compile error
- Pattern match type checking (arm types must unify)

### Phase 3: Annotations + Error Reporting
- Parse and store type annotations from reader_ta.c
- Check annotations against inferred types
- Clear error messages with function name and type info
- Polymorphic generalization for top-level functions
- Test suite for type system

## File Plan
- NEW: `src/typecheck.h` — Type representation and API
- NEW: `src/typecheck.c` — Type checker implementation
- MODIFY: `src/reader_ta.c` — capture type annotations in AST
- MODIFY: `src/compile.c` — call type checker before codegen
- MODIFY: `Makefile` — add typecheck.c to SRC
- MODIFY: `ta.h` — forward declarations if needed