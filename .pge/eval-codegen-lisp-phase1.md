# Eval Report: codegen.lisp — Phase 1

**Result: PASS** (5/5 criteria fully met)

**Evaluator:** Independent (did NOT write the code)
**Date:** 2025-06-28

---

## Acceptance Criteria

### ✅ 1. codegen.lisp file exists, uses S-expr Lisp syntax

**Evidence:** `lib/codegen.lisp` exists (375 lines). The entire file is written in
S-expression Lisp syntax compatible with the tinyactor reader. It defines functions
using `(define (name args...) body)`, uses `let`, `if`, `cons`, `car`, `cdr`, etc.
No non-Lisp syntax found. The file is a translation of `codegen.ta` into Lisp form.

### ✅ 2. codegen.lisp can be compiled by reader.c + compile.c (runs without error)

**Evidence:** Concatenated `lib/codegen.lisp` with a test driver and ran:
```bash
cat lib/codegen.lisp test_driver.lisp > /tmp/run.lisp
./tinyactor /tmp/run.lisp
```
Exit code 0, no errors. The 8 MiB heap increase in `src/reader.c` was necessary
and sufficient to load and execute the 375-line module.

### ✅ 3. codegen.lisp can compile a simple .lisp AST to produce correct .tabc

**Evidence — three independent test programs, all verified byte-for-byte:**

| Test | Description | C code section | Lisp code section | Match? |
|------|-------------|----------------|-------------------|--------|
| `simple_let` | `let x=3; let y=4; print(x+y)` | 44 bytes | 44 bytes | ✅ identical |
| `multi_fn` | `fn add(x,y){x+y}; fn main(){print(add(3,4))}` | 54 bytes | 54 bytes | ✅ identical |
| `if_string` | `let x=5; if(x<10){print("yes")}else{print("no")}` | 60 bytes | 60 bytes | ✅ identical |

Each Lisp-produced `.tabc` was also loaded and executed by the VM, producing the
correct output (7, 7, "yes" respectively) — matching the C-compiled `.tabc` behavior.

Method: The AST was constructed using explicit `cons` calls (since the tinyactor
reader does not support quoted list literals — see Notes). The code section
(last `code_size` bytes of each `.tabc`) was extracted and compared with Python.

### ✅ 4. All 60 existing tests still pass

**Evidence:** `make test` output:
```
Total:   60
Passed:  60
Failed:  0
```
All `.lisp` and `.ta` test scripts pass.

### ✅ 5. No set! or mutable variables introduced

**Evidence:** `grep` for `set!`, `setq`, `mutable`, `var` in `lib/codegen.lisp`
returns only ONE hit: line 65, where `"set!"` appears as a **string literal**
in the symbol table (`init_syms`), registering it as symbol ID 41. This is
a lookup-table entry, NOT an actual use of mutation. No `set!` special form
is ever invoked in codegen.lisp. All state is threaded through function
return values (functional style: state is passed and returned explicitly).

---

## Additional Checks

### src/reader.c change — Reasonable ✅

**Diff:**
```c
- sp->mem_size = 32768;
+ sp->mem_size = 1 << 23;        /* 8 MiB */
  sp->mem = malloc(sp->mem_size);
+ sp->gc_to = malloc(sp->mem_size);
```

**Assessment:** Two changes:
1. **Heap size 32 KB → 8 MiB**: Needed because `codegen.lisp` (375 lines of
   deeply nested Lisp) requires more heap than 32 KB when loaded and evaluated.
   Without this, the reader runs out of memory.
2. **`gc_to` allocation added**: The scratch `Proc` in `get_scratch()` previously
   did NOT allocate `gc_to`. If GC triggered during compilation of a large file,
   it would write to a NULL pointer (crash). The VM's `vm_new_proc` (vm.c:295)
   already allocates `gc_to`, so this brings the scratch proc in line. **This is
   a latent bug fix, not just a size increase.**

The indentation has extra whitespace (cosmetic, not functional). No breakage —
all 60 tests pass with this change.

---

## Notes / Observations (not failures)

1. **Quoted list literals unsupported in the host language**: `(quote (a b c))`
   compiles to `OP_PUSH_NIL` in the C compiler (compile.c:804-830 — the `else`
   branch). This means you cannot write `'((define (main) ...))` to pass an AST
   to `compile_and_write`. The AST must be built with explicit `cons` calls.
   This is a limitation of the **host language reader/compiler**, not of
   `codegen.lisp` itself. The acceptance criteria ask for codegen.lisp to compile
   an AST — it does so correctly. But it makes ad-hoc testing more verbose.

2. **Symbol table strategy differs but is compatible**: The C compiler adds local
   variable names (e.g., `x`, `y`) to the symbol table and uses `PUSH_SYM` for
   lookups in some contexts. `codegen.lisp` uses `LOAD_LOCAL` (slot-based) for
   locals and does not add them to the symbol table. Despite this, the **code
   sections are byte-for-byte identical** because both compilers use `LOAD_LOCAL`
   / `STORE_LOCAL` for the actual variable access in function bodies. The symbol
   table difference only affects the header/symbol-table region, not the code.

3. **Code quality**: The code is clean, well-commented, organized into clear
   sections (byte emission, symbol table, expression compilation, passes,
   serialization, public API). It documents the fixes vs. the original
   `codegen.ta` (corrected opcode numbers for PRINT/ENTER/HALT/SPAWN_MAIN,
   CALL nargs as u32, CLOSURE for function references).

---

## Summary

**5/5 criteria PASS.** `codegen.lisp` is a correct Lisp-syntax compiler that
produces bytecode byte-for-byte identical to the C compiler for the tested
programs (arithmetic, multi-function calls, if/else with strings). All 60
existing tests pass. No mutable state is introduced. The `src/reader.c` change
is a necessary and correct fix (heap size + missing `gc_to` allocation).