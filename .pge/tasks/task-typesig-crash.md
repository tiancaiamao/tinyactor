# Task A6: Type Annotation Enforcement via type-sig Emission

## Problem
Type annotations (`fn foo(x: int) -> string { ... }`) are captured by the C reader but NOT emitted into the AST as `type-sig` forms. The TA typecheck never sees them, so **type annotations are silently ignored**. Example: `fn bad_fn() -> string { 42 }` produces no type error.

## Root Cause
The C reader (`src/reader_ta.c`, `parse_toplevel_fn` function around line 987) captures type annotations into the `FnAnnotation` struct (for the now-unused C typecheck), but only returns `(define (name params...) body...)`. It does NOT emit `(type-sig name (param_types...) ret_type)` forms like the TA parser does.

The TA typecheck (`lib/typecheck.ta`) relies on `collect_type_sigs` to find `type-sig` forms in the AST. Without them, annotations are invisible to the type checker.

Additionally, `type-sig` forms (if they appeared in the AST) are not handled by the C compiler (`src/compile.c`). They'd crash if encountered as top-level expressions.

## What to Fix (3 changes)

### Change 1: `src/reader_ta.c` — Emit type-sig forms
In `parse_toplevel_fn` (around line 1077), when type annotations exist, return `(begin (type-sig ...) (define ...))` instead of just `(define ...)`.

The type-sig form format must be: `(type-sig name_sym (param_type1 param_type2 ...) ret_type)`
- `name_sym`: the function name as a Val symbol
- param_types: a list where each element is either a type symbol (like `'int`, `'string`) or nil (for untyped params)
- ret_type: a type symbol or nil

The annotations are currently captured as C strings in `anno->param_types[i]` and `anno->ret_type`. These need to be converted to TA symbols. For simple types like "int", "string", "bool", "pid" — these are already valid TA symbols. For compound types like "List(int)" — these would need to be parsed, but for now just emit them as symbols (the typecheck's `parse_type_annot` handles compound types if they're in list form).

**Important**: A parameter that has no annotation should be `nil` in the param_types list. A parameter with annotation "int" should be the symbol `'int`.

Example: `fn foo(x: int, y, z: string) -> bool { ... }` should produce:
`(type-sig foo (int nil string) bool)`

### Change 2: `src/api.c` — Flatten begin forms at top level
In `parse_source` (around line 234), when appending a form to the forms list, check if the form is `(begin form1 form2 ...)` and if so, append each subform individually. This ensures the `type-sig` and `define` forms from Change 1 become separate top-level forms.

### Change 3: `src/compile.c` — Handle type-sig as no-op
Add `type-sig` to the skip list in two places:
1. **Expression compilation** (~line 845): Add alongside the existing `type` handling:
```c
if (sym_eq(c->vm, head, "type-sig")) {
    emit_byte(&c->code, OP_PUSH_NIL);
    return;
}
```
2. **Top-level form processing** (~line 1420): Add `is_type_sig` check alongside `is_import` and `is_type`:
```c
int is_type_sig = val_is_pair(form) && sym_eq(vm, ast_car(form), "type-sig");
// Update: if (!is_import && !is_type && !is_type_sig) has_top = 1;
```

## Files to Modify
1. `src/reader_ta.c` — `parse_toplevel_fn`: emit `(begin (type-sig ...) (define ...))` when annotations present
2. `src/api.c` — `parse_source`: flatten `(begin ...)` at top level
3. `src/compile.c` — handle `type-sig` as no-op in expression + top-level

## Acceptance Criteria
1. `fn bad_fn() -> string { 42 }` triggers a type error with `--check`
2. `fn good_fn(x: int) -> int { x + 1 }` passes with no errors
3. Programs without annotations work unchanged (backward compatible)
4. All existing tests pass (`make && bash test/run_all_tests.sh`)

## Verify Commands
```bash
# 1. Type mismatch detected
cat > /tmp/test-bad-annot.ta << 'EOF'
fn bad_fn() -> string {
  42
}
fn main() {
  print("PASS")
}
EOF
NWORKERS=1 ./tinyactor --bootstrap /tmp/test-bad-annot.ta '' --check 2>&1
# Should show a type error mentioning string vs int

# 2. Correct annotation passes
cat > /tmp/test-good-annot.ta << 'EOF'
fn good_fn(x: int) -> int {
  x + 1
}
fn main() {
  print("PASS")
}
EOF
NWORKERS=1 ./tinyactor --bootstrap /tmp/test-good-annot.ta '' --check 2>&1
# Should print PASS, no type errors

# 3. All tests pass
make && bash test/run_all_tests.sh 2>&1 | tail -5
```

## Key Context
- `reader_ta.c` line 987: `parse_toplevel_fn` — where function parsing happens
- `reader_ta.c` line 1019: param type annotation capture (C strings in `anno->param_types[i]`)
- `reader_ta.c` line 1053: return type annotation capture (C string in `anno->ret_type`)
- `api.c` line 234: `parse_source` — top-level form collection loop
- `compile.c` line 845: `type` form handling (add `type-sig` alongside)
- `compile.c` line 1420: top-level skip list (add `type-sig`)
- `typecheck.ta` line 1127: `collect_type_sigs` — processes type-sig forms
- `typecheck.ta` line 1062: `parse_type_annot` — converts symbols to types
- The `intern_sym(vm, name, len)` function creates TA symbols from C strings