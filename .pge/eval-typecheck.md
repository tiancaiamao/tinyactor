# Type System Implementation — Evaluation Report

**Evaluator:** Independent (did not write the code)
**Commit:** `a81d218 feat: Hindley-Milner type checker with polymorphism`
**Date:** Eval run on top of `56fde89` (pre-typecheck) compared with `a81d218` (with typecheck)

---

## Result: ✅ **PASS**

All acceptance criteria verified independently. The Hindley-Milner type checker is correctly integrated, catches real type errors, preserves polymorphism, and does not regress existing functionality. Three flaky test failures observed during the run are pre-existing concurrency/network/timing flakiness (present with and without typecheck), NOT caused by this change.

---

## L1 — Structural Criteria

### ✅ `src/typecheck.h` exists with Type representation
- File exists (1893 bytes). Defines `TypeKind { TY_VAR, TY_CON, TY_ARROW, TY_ADT }`, a recursive `Type` struct with union for var/con/arrow/adt, `TypeCtx`, `FnAnnotation` struct (name, param_types[16][64], ret_type, has_ret_annotation), and `int typecheck_program(VM*, Val forms)`. Clean HM design.

### ✅ `src/typecheck.c` exists and compiles
- File exists (1118 lines). Compiled cleanly with `cc -Wall -Wextra -std=c99 -O2`. The only warnings in the build come from pre-existing files (api.c unused `read_u32`, net.c unused `vm` params) — none from typecheck.c.

### ✅ `make` passes with typecheck.c added to Makefile
- `make clean && make` produces `tinyactor` (144168 bytes). Makefile `SRC` includes `src/typecheck.c`. Object rule `%.o: %.c ta.h` covers it; `src/typecheck.o` builds.

### ✅ reader_ta.c captures type annotations (param types, return types)
- `grep` confirms: `#include "typecheck.h"`; `static FnAnnotation fn_annotations[256]`; `reader_get_annotations()` / `reader_reset_annotations()`. Param annotation capture at lines 1022–1042 (`: Type` after each param identifier, terminated by `,` or `)`). Return annotation capture at lines 1048–1064 (`-> Type` before `{`). `anno->nparams`, `anno->has_ret_annotation` populated.
- Behavioral confirmation: `fn double(x: int) -> int { x*2 }` runs correctly and `fn make(x: int) -> string { x }` is rejected with `Type error: argument type mismatch: int vs string`.

### ✅ Type checker is called in compile pipeline before code generation
- `src/compile.c:1288`: `int compile_all(VM *vm, Val forms)` calls `if (typecheck_program(vm, forms) != 0) return -1;` as the very first statement, before `Compiler c; comp_init(&c, vm);` and Phase 1 function registration. Verified by direct read.

---

## L2 — Behavioral Criteria

### ✅ Type inference: `fn add(x, y) { x + y }` → inferred `(int, int) → int`, works at runtime
- `/tmp/tc1.ta`: `fn add(x,y){x+y}` + `print(add(1,2))` → prints `3`, exit 0.
- Inference path: `x + y` unifies both operands to `int` (per `unify_arith`), return type unified to `int`. Confirmed at runtime.

### ✅ Type checking catches: `fn bad(x: int) { x + true }` → compile error
- `/tmp/tc2.ta` → `Type error: arithmetic operand is not int: bool vs int` + `error: failed to load`, **exit 1** (verified directly, not through a pipe). The annotation `x: int` is enforced and `true` (bool) is rejected.

### ✅ ADT checking: constructors with payload work at runtime
- `/tmp/tc_adt.ta` (custom test):
  ```
  type Tree { Leaf(int); Node(Tree, Tree) }
  fn sum(t) { match t { Leaf(n)->n; Node(l,r)->sum(l)+sum(r) } }
  fn main() { let t = Node(Leaf(1), Node(Leaf(2), Leaf(3))); print(sum(t)) }
  ```
  → prints `6`, exit 0. Recursive ADT with payload constructors + recursive match works.

### ✅ Pattern match with constructors works
- `test/scripts/match_test.ta` → `1\n2\n3\n4`, exit 0 (literal, symbol, `cons(a,b)` constructor destructure, `nil`).
- `test/scripts/adt-basic.ta` → `red\ngreen\nblue\nPASS`, exit 0 (no-payload constructor match).
- `pattern-match-in-actor.ta`, `pattern-match-pair-destructure.ta` exist and are exercised by the suite.

### ✅ Polymorphism: `fn id(x) { x }` works with both int and string
- `/tmp/tc3.ta`: `fn id(x){x}` + `print(id(42)); print(id("hello"))` → prints `42\nhello`, exit 0.
- Mechanism verified in source: `generalize_type_excluding` runs after each `check_define`, marking env-free vars as `quantified`; `env_lookup` calls `instantiate` to fresh-copy quantified vars on each use. The single `id` binding serves both call sites with fresh type variables.

### ✅ All existing tests still pass (no behavioral regression)
- `make test` total = 180. Pass count observed across multiple runs: 177 / 176 / 179. The 1–4 failures per run are exclusively from the concurrent/network/bootstrap flaky set:
  - `echo_test.ta` (network server with port binding) — sometimes TIMEOUT
  - `monitor_test.ta` (down-message timing) — sometimes NO OUTPUT (ran 5× directly: 4 had output, 1 didn't)
  - `error-send-to-dead.ta`, its bootstrap + bytecode-cmp variants — depend on cross-process race
- **Regression check on parent commit `56fde89` (typecheck removed):** `make test` → 178 pass / 2 fail, with a *different* failing subset (`monitor_test` basic NO OUTPUT, `bytecode-cmp error-send-to-dead` output differs). The failure set is non-deterministic and the tests that fail with-typecheck are not failing without-typecheck in a stable way — they swap. This is environmental flakiness in the concurrent/network tests, not a typecheck regression.
- All deterministic core tests (arith, closure, adt, gc, ping_pong, match, etc.) pass consistently with typecheck enabled.

### ✅ Type annotations are optional: code without annotations still works
- `test/scripts/ping_pong.ta` → `ping done\npong done\nall done`, exit 0.
- `test/scripts/adt-basic.ta` → PASS (no annotations on `color_name`).
- `test/scripts/gc-closure-churn.ta` → `42`, exit 0.
- The type checker is permissive by design (HM inference); untyped code gets type variables that unify freely.

### ✅ Type error: arithmetic on non-int types caught
- `"x" + 1` → `Type error: arithmetic operand is not int: string vs int`, exit 1.
- `true + false` → `Type error: arithmetic operand is not int: bool vs int`, exit 1.
- Unification of `+` operands forces `int`; any non-int operand is rejected at compile time.

---

## Memory Safety Review

- **Allocation tracking:** All `Type` objects are registered in a static `g_alloced[]` array (capacity-doubled) and freed once via `type_free_all()` at the end of `typecheck_program`. No double-free: each Type is freed exactly once; `arrow.params` and `adt.args` sub-arrays are freed per-Type before `free(t)`.
- **Env lifecycle:** Every `env_new()` call (global, fn, lambda, let×2) is paired with `env_free_node()`. No env leak. `env_free_node` correctly frees only the current node (parents freed separately).
- **Minor robustness gap (non-blocking):** `type_alloc` does not check the `realloc` return value for `g_alloced`; on OOM this would be a NULL deref. Given `g_alloc_cap` starts at 512 and only OOM could trigger this, it is acceptable for a one-shot compiler. No use-after-free or leak observed in the happy path.
- **String lifetime:** `con.name` / `adt.name` are `intern_sym`-backed (VM-managed, live for program duration) — safe to reference without copying.

---

## Issues Found (non-blocking)

1. **Flaky concurrent tests (pre-existing, not typecheck-related):** `echo_test`, `monitor_test`, `error-send-to-dead` and their bootstrap/bytecode-cmp variants occasionally fail due to process scheduling / port races. Suggest adding deterministic synchronization or marking them expected-flaky. Not a blocker for this task.
2. **`type_alloc` realloc return unchecked** — add `if (!g_alloced) { /* handle OOM */ }` for defensive robustness. Cosmetic.
3. **`FnAnnotation` fixed-size buffers:** `name[128]`, `param_types[16][64]`, `ret_type[64]` are clamped (truncation is safe, length checks present at lines 1031–1036 / 1058–1061), so no overflow. Acceptable.

---

## Verdict

**PASS.** All L1 structural and L2 behavioral acceptance criteria are met with independent evidence. The HM type checker integrates cleanly, infers polymorphic types, enforces annotations, and catches arithmetic/mismatch errors without regressing the existing deterministic test suite.