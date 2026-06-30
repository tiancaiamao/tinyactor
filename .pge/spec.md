# Spec: Type Checker Self-Hosted in .ta

## Goal
Port the HM type checker from C (typecheck.c) to .ta itself, inspired by Cora's infer.cora.
This completes the bootstrap story and proves the language is powerful enough for metaprogramming.

## Design Principle
- **No mutable state** (Gleam doesn't have it, neither should we)
- **Substitution-based unification** (Algorithm W style, not union-find)
- **State threading**: counter and env passed as function parameters

## Type Representation (pair-tree)
- Base types: `'int`, `'bool`, `'string`, `'symbol`, `'pid`
- Type variables: integers (1000, 1001, ...)
- Function types: `[ParamType '-> RetType]` — curried, one arg at a time
- Lists: `['list ElemType]`
- Substitution: assoc list `[[VarNum . Type] ...]`

## Language Features to Add
1. `symbol?` — OP_IS_SYM in VM (check TAG_SYM)
2. `bool?` — OP_IS_BOOL in VM (check val_true()/val_false())

## Architecture
```
lib/typecheck.ta
├── assq(key, alist)         — assoc list lookup (pure .ta)
├── apply_subst(type, subst) — resolve type variables
├── occur_check(var, type)   — occurs check for recursion prevention
├── unify(t1, t2, subst)     → ['succ subst] | ['fail reason]
├── check_type(expr, type, local_env, global_env, subst, counter) → result
└── check_program(ast)       — entry point
```

## State Threading
Instead of mutable globals (Cora's `*tvar*`, `*type-env*`):
- `counter`: threaded through function returns as `[new_counter, result]`
- `global_env`: passed as parameter, updated via cons
- `subst`: threaded via bind_s continuation pattern

## File Plan
- MODIFY: `ta.h` — add OP_IS_SYM, OP_IS_BOOL
- MODIFY: `src/vm.c` — implement OP_IS_SYM, OP_IS_BOOL
- MODIFY: `src/compile.c` — add to inline_ops table
- NEW: `lib/typecheck.ta` — the type checker (self-hosted)
- NEW: `test/scripts/typecheck-self-test.ta` — test the .ta type checker

## Phases
1. Add `symbol?` and `bool?` to VM
2. Write utility functions (assq, etc.)
3. Write unify, apply_subst, occur_check
4. Write check_type (the core inference)
5. Write test harness
6. Integration: figure out how to run .ta type checker during compilation