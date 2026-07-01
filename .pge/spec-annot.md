# Spec: Phase 3 — Type Annotation Preservation + Validation

## Goal
Modify the parser to preserve type annotations (`fn foo(x: int) -> int`) in the AST, then use them in the type checker for annotation validation (check that inferred types match declared annotations).

## Current State
- Parser **strips** annotations: `parse_single_param` skips `: Type`, `parse_fn` skips `-> Type`
- Tokenizer already recognizes `:` as `'colon` and `->` as `'arrow`
- Type checker has no access to annotation info
- Only `test/scripts/type-pass.ta` uses annotations in practice

## Approach: `type-sig` Top-Level Form

When the parser encounters a function WITH type annotations, it emits an extra `type-sig` form alongside the `define`:

```ta
// Source:
fn add(x: int, y: int) -> int { x + y }

// Parser AST output:
(type-sig add (int int) int)    ← NEW: annotation metadata
(define (add x y) (+ x y))      ← unchanged define form
```

**Rules:**
- `type-sig` is ONLY emitted if there's at least one annotation (param or return)
- Unannotated params get `nil` in the param type list
- No return annotation → return type is `nil`
- Programs without annotations are completely unaffected

**Codegen:** `type-sig` forms are skipped (same as `import` and `type`)

**Type checker:** `type-sig` forms provide expected types for unification

## Type Annotation Format

```
(type-sig func_name (param_type1 param_type2 ...) return_type)
```

- `func_name`: symbol (e.g. `'add`)
- `param_typeN`: symbol (`'int`, `'string`, `'bool`, `'pid`, `'Pid`, custom ADT name) or `nil` if unannotated
- `return_type`: symbol or `nil`

For Phase 3, only **simple named types** are supported. Compound types (`List(int)`, arrows) are future work.

## Files to Modify

### 1. `lib/parser.ta`

**`parse_single_param`** — capture the type annotation instead of skipping it:
- Current: skips `: Type`, returns `(name_sym . pos)`
- New: returns `(name_sym . (type_sym . pos))` where `type_sym` is the annotation symbol or `nil`
- BUT: this changes the return format, affecting `parse_params_loop`
- **Better approach**: return `(annotated_param . pos)` where `annotated_param` = `(name_sym . type_sym)` or just `name_sym` if no annotation. Then `parse_fn` extracts both the plain param list (for the define form) and the type list (for type-sig).

**`parse_fn`** — capture return type annotation:
- After parsing params and before `skip_to_lbrace`, check for `-> Type`
- If found, capture the type symbol
- After building the define form, if any annotations exist, also emit a `type-sig` form

**Output of `parse_fn`** changes from `(form . pos)` to `(form_or_pair . pos)`:
- If annotations: return `((type-sig ...) (define ...))` — a pair of forms
- If no annotations: return `(define ...)` — single form (unchanged)
- Callers that consume `parse_fn` output need to handle the possibility of multiple forms

Wait — this changes the return format which affects callers. **Simpler alternative**: emit `type-sig` as a separate top-level form BEFORE the define form:
```
(parse_top_level returns: ((type-sig ...) . ((define ...) . nil)))
```

Actually, the simplest approach: make `parse_fn` return a LIST of forms (1 or 2 elements). Then `parse_program` / `parse_top_level` handles flattening.

Let me look at how parse_fn is called:

Currently `parse_fn` returns `(form . pos)`. The caller is `parse_top_level` which conses the form into the AST.

**Cleanest approach**: `parse_fn` returns `(forms_list . pos)` where `forms_list` is a list of 1 or 2 forms. Caller flattens it.

### 2. `lib/codegen.lisp`

Two places need to skip `type-sig`:
- `compile_top_level` (line ~1051): add `type-sig` to the skip conditions
- `pass1_register` (line ~963): add `type-sig` to skip conditions (so it doesn't register as a function)

Each is a one-line addition.

### 3. `lib/typecheck.ta`

**New function: `parse_type_annot(sym)`** — converts annotation symbol to internal type:
- `'int` → `t_int()` = `(base int)`
- `'string` → `t_string()` = `(base string)`
- `'bool` → `t_bool()` = `(base bool)`
- `'pid` → `t_pid()` = `(base pid)`
- `'Pid` → `t_pid()` = `(base pid)` (capitalize variant)
- Anything else → `t_base(sym)` (custom ADT type name)
- `nil` → `nil` (no annotation)

**New function: `collect_type_sigs(forms, env, counter)`** — walks forms, for each `(type-sig name param_types ret_type)`:
- Convert param_types to internal types
- Convert ret_type to internal type
- Build arrow type: `param1_t -> param2_t -> ... -> ret_t`
- If ret_type is nil, use fresh var for return
- For nil param types, use fresh var
- Wrap in forall if there are any free vars
- Register in env as annotation (separate alist or extend env with a special marker)

**Modify `infer_define`** — after inferring a function's type, check if there's a matching type-sig and unify:
- If annotation exists for this function, unify inferred type with annotated type
- If unification fails, it's a type error (but for now, just silently accept — be permissive)

**New test functions:**
- `test_j()`: Annotated function — `fn add(x: int, y: int) -> int` → inferred type matches annotation
- `test_k()`: Partially annotated function — `fn id(x: int)` → `int -> 'a` or similar
- `test_l()`: Type-sig with custom ADT — annotate with ADT type name

## Acceptance Criteria

### L1 — Structural
- [ ] `make -j4` succeeds (codegen.lisp change doesn't break C build)
- [ ] `./tinyactor lib/typecheck.ta` runs and exits 0
- [ ] `./tinyactor lib/parser.ta` runs and exits 0 (parser change is self-consistent)
- [ ] Parser emits `type-sig` forms when annotations present — Verify: parse a function with annotations and check AST
- [ ] Codegen skips `type-sig` forms — Verify: compile a program with annotations without error

### L2 — Behavioral
- [ ] Test J: Annotated function type-checks correctly (types match annotation)
- [ ] Test K: Partially annotated function type-checks
- [ ] All existing typecheck.ta tests (1-7, A-I) still pass
- [ ] `test/scripts/type-pass.ta` compiles successfully with the updated parser + codegen
- [ ] Bootstrap still works: `./tinyactor lib/driver.ta` runs without error

## Constraints
- Modify 3 files: `lib/parser.ta`, `lib/codegen.lisp`, `lib/typecheck.ta`
- NO `||` operator (broken in .ta)
- NO `!` operator (use `== false`)
- Functions top-level only in .ta files
- Keep functions under 30 statements (parser `items[64]` limit)
- Parser functions use `match` for pattern matching
- Codegen is in Lisp syntax (not .ta)
- **Bootstrap must still work** — parser.ta must be parseable by the current parser, and the bootstrap pipeline must not break

## Out of Scope
- Compound type annotations (`List(int)`, `(int, string)`)
- Arrow type annotations (`int -> int` as a param type)
- Type error reporting (just silently accept mismatches for now)
- Module function type checking (imported module's functions)