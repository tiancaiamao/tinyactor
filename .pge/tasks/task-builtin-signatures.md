# Task A8: Add More Builtin Function Type Signatures

## Objective

Add type signatures for commonly used builtin functions to `make_builtin_env` in `lib/typecheck.ta`, so the type checker can properly type-check code that calls them.

## Context

The `make_builtin_env` function (around line 1268 in `lib/typecheck.ta`) currently registers type schemes for these builtins:
- Core: car, cdr, cons, print, null?, pair?, int?, string?, symbol?, len
- Strings: str_concat, str.eq, str.concat, str.length
- Actor: spawn, send, recv, self, monitor, receive-scan, recv-commit, recv-skip
- Misc: list_ref

## Missing Builtins to Add

These are C-level builtin functions registered in the VM that currently lack type signatures. When user code calls these, they resolve as "unbound" in the type checker and get fresh type variables (no type checking possible).

### str module (from src/str.c)
| Function | Type Signature |
|----------|---------------|
| `str.char_at` | `string -> int -> int` |
| `str.substr` | `string -> int -> int -> string` |
| `str.to_int` | `string -> int` |
| `str.from_int` | `int -> string` |
| `str.index_of` | `string -> string -> int` |
| `str.to_sym` | `string -> 'a` (returns symbol, but we don't have a symbol base type — use fresh var) |
| `str.sym_to_str` | `'a -> string` (accepts symbol, but use fresh var for permissiveness) |

### Core builtins (from C VM, used as bare symbols)
| Function | Type Signature |
|----------|---------------|
| `+`, `-`, `*`, `/` | `int -> int -> int` |
| `<`, `>`, `<=`, `>=` | `int -> int -> bool` |
| `==` | `forall(a, a -> a -> bool)` |
| `not` | `bool -> bool` |
| `list` | `forall(a, a -> 'a)` (variadic, but typecheck sees 1+ args — just make it permissive: `forall(a b, a -> b)`) |

**Note:** `list` is variadic, so giving it a precise type is tricky. Skip it if uncertain.

## Implementation Details

All additions go in the `make_builtin_env(counter)` function in `lib/typecheck.ta`. The current function builds up env using chained `extend` calls (e1, e2, ..., e23). Continue the pattern with e24, e25, etc.

Helper constructors already available:
- `t_int()` → `(base int)`
- `t_bool()` → `(base bool)`  
- `t_string()` → `(base string)`
- `t_pid()` → `(base pid)`
- `t_arrow(from, to)` → `(arrow from . to)`
- `t_var(id, level)` → `(tvar id . level)`
- `t_forall(vars, type)` → `(forall vars . type)`
- `fresh(counter, level)` → `(new_id . new_counter)`
- `extend(env, name, scheme)` → new env

## Build & Test

```bash
# Bootstrap (rebuilds typecheck.tabc from typecheck.ta source)
make bootstrap

# Self-test (typecheck.ta has embedded self-tests)
NWORKERS=1 ./tinyactor --bootstrap lib/typecheck.ta '' --check

# Error test
NWORKERS=1 ./tinyactor --bootstrap test/scripts/typecheck-errors.ta '' --check

# Full test suite
make test
```

## Acceptance Criteria

1. `make bootstrap` succeeds without errors
2. `NWORKERS=1 ./tinyactor --bootstrap lib/typecheck.ta '' --check` runs and outputs all self-test results
3. `NWORKERS=1 ./tinyactor --bootstrap test/scripts/typecheck-errors.ta '' --check` still passes (no regression)
4. `make test` passes with same baseline (186/188, 2 pre-existing flaky tests)
5. The type signatures are correctly structured (use existing helpers, match the function arity)
6. No DEBUG prints left in the code

## File Scope

- ONLY modify: `lib/typecheck.ta` (the `make_builtin_env` function)
- Do NOT modify any C files