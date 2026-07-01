# Task: Parser + Codegen — Preserve Type Annotations

## Files to modify
1. `lib/parser.ta` — capture type annotations, emit `type-sig` forms
2. `lib/codegen.lisp` — skip `type-sig` forms

## Objective

When parsing a function WITH type annotations like `fn add(x: int, y: int) -> int { x + y }`, the parser should emit BOTH:
```
(type-sig add (int int) int)         ← type annotation metadata
(define (add x y) (+ x y))           ← unchanged define form
```

When parsing a function WITHOUT annotations like `fn id(x) { x }`, the parser should emit ONLY:
```
(define (id x) x)                    ← unchanged, no type-sig
```

### `type-sig` Format
```
(type-sig func_name (param_type1 param_type2 ...) return_type)
```
- `func_name`: symbol
- Each `param_typeN`: symbol (`'int`, `'string`, `'bool`, `'pid`, custom ADT name) or `nil` if unannotated
- `return_type`: symbol or `nil` if no return annotation

### Rules
- Only emit `type-sig` if at least one annotation exists (param or return)
- Programs without annotations produce identical output to before (no `type-sig` forms)

## Current Parser Behavior (to be changed)

Key functions in `lib/parser.ta`:

**`parse_single_param` (line 263):** Currently skips `: Type` via `skip_type()`. Returns `(name_sym . pos)`. Needs to capture the type symbol instead of skipping it.

**`parse_params_loop` (line 245):** Collects params into a list. Currently returns `(param_list . pos)`. Needs to also collect type annotations alongside param names.

**`parse_fn` (line 833):** Currently uses `skip_to_lbrace` to skip `-> Type`. Needs to capture the return type. Needs to conditionally emit a `type-sig` form.

**`parse_toplevel` (line 788):** Returns `(form . pos)`. `parse_fn` now may return 1 or 2 forms. The caller needs to handle multiple forms.

**`parse_toplevels` (line 774):** Main loop. Calls `parse_toplevel`, conses form into accumulator. Needs to handle multiple forms from `parse_toplevel`.

### Token info
- `:` has token type `'colon` (tokenizer line 256, char code 58)
- `->` has token type `'arrow` (tokenizer line 263)
- Type names like `int`, `string` are regular ident tokens
- `tval(toks, pos)` gets token value, `ttype(toks, pos)` gets token type
- `is_type(toks, pos, 'colon)` checks if token at pos is a colon
- `str.to_sym(string)` converts string to symbol

### Important TA language constraints
- NO `set` or mutation — all bindings are immutable. Use nested `let` or helper functions instead.
- NO `||` operator (broken in TA)
- NO `!` operator
- Functions must be top-level only
- Use `match` for pattern matching
- Keep function bodies under 30 statements (parser `items[64]` limit)

## Codegen Changes (`lib/codegen.lisp`)

The codegen is written in Lisp syntax (not .ta). Two functions need to skip `type-sig` forms:

1. **`pass1_register` (line ~963):** Skips non-define forms during function registration. Add `type-sig` to the skip conditions (same as how `import` and `type` are skipped).

2. **`compile_top_level` (line ~1051):** Skips `import` and `type` forms during compilation. Add `type-sig` to the skip list — it should recurse to the next form without emitting any bytecode.

Look at how `import` and `type` are handled in these functions and add `type-sig` the same way.

## Verification Commands

```bash
# Parser self-test
./tinyactor lib/parser.ta

# Compile a program WITH annotations (should work)
./tinyactor test/scripts/type-pass.ta

# Bootstrap still works
./tinyactor lib/driver.ta

# Type checker still works
./tinyactor lib/typecheck.ta
```

All must exit 0 without errors.

## Key Design Decision: Return Format

`parse_fn` currently returns `(form . pos)`. Since it now may produce 1 or 2 forms, the simplest approach:
- `parse_fn` returns `(form_list . pos)` where `form_list` is always a proper list (1 or 2 elements)
- `parse_toplevel` wraps non-`parse_fn` results in a list too: `(cons(single_form, nil) . pos)`
- `parse_toplevels` prepends all forms from the list to the accumulator

You'll need a helper like:
```ta
fn prepend_all(forms, acc) {
  match forms {
    nil -> acc
    cons(f, rest) -> cons(f, prepend_all(rest, acc))
  }
}
```