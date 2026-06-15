# Phase 6a Evaluation — ADT + Constructor Syntax

Commit evaluated: `95aae67` (against parent `251bf18`)
Branch tip diff: `git diff 95aae67~1 --stat`

## L1 — Structural

### 1. `make clean && make` — 0 errors  ✅
```
cc -Wall -Wextra -std=c99 -O2 -I. -o tinyactor ... -lpthread
```
Build completes. Only pre-existing warnings (`env_snapshot` unused in compile.c, `vm` unused in net.c) — **0 errors**.

### 2. `src/reader_ta.c` has constructor table + `type` keyword handling  ✅
- Constructor table: `static char constructors[256][64]; static int n_constructors;`
- Detection: `is_constructor()` (registry lookup) + `is_upper_ident()` (uppercase fallback)
- `parse_toplevel_type()` parses `type Name { V1(T); V2 }`, registers variants, emits `(type)` no-op form
- Dispatch wired into `reader_ta_read()` via `is_keyword(&lx, "type")`

### 3. `src/compile.c` skips `(type)` forms  ✅
```c
/* (type) — ADT declaration form; no-op at runtime */
if (sym_eq(c->vm, head, "type")) {
    emit_byte(&c->code, OP_PUSH_NIL);
    return;
}
```
Also excluded from `has_top` in `compile_all()`: `if (!is_import && !is_type) has_top = 1;`

### 4. No changes to forbidden files  ✅
```
 src/compile.c   |  13 +++-
 src/reader_ta.c | 183 +++++++++++++++++++++++++++++++++++++++++++++++++++++---
```
Only `compile.c` + `reader_ta.c`. **vm.c, gc.c, ta.h, val.c, reader.c, main.c untouched.**

### 5. All 49 .lisp tests pass  ✅
Regression loop (`grep -qiE "FAIL|Segmentation|Error"`):
```
lisp-done fails=[]
```
0 failures across 49 `.lisp` files.

### 6. All 5+1 existing .ta tests pass  ✅
| Test | Output (tail) |
|---|---|
| hello.ta | `hello` |
| arith.ta | `7` |
| multithread-basic.ta | `(alpha string . hello)` ... `PASS` |
| recv-scan.ta | `got-second` / `then-first` / `PASS` |
| echo_test.ta | `PASS` |
| adt-basic.ta | `red` / `green` / `blue` / `PASS` |

All 6 pass.

## L2 — Behavioral

### 7. Nullary ADT  ✅
```
type Color { Red; Green; Blue }
let c = Red
match c { Red -> print("red") ... }
```
→ outputs `red`. Exit 0.

### 8. N-ary ADT with binding  ✅
```
type Msg { Hello(Pid, String); Bye }
let msg = Hello(self(), "world")
match msg { Hello(from, text) -> { print("hello"); print(text) } ... }
```
→ `hello` / `world`. Exit 0.

### 9. Actor messaging with ADT  ✅
```
type Msg { Ping(Pid); Pong; Stop }
```
Spawned ponger receives `Ping(from)`, sends back `Pong`. Main receives `Pong`, sends `Stop`.
→ `got pong` / `ok`. Exit 0.

### 10. Type annotations  ✅
```
fn f(x: Int, y: Int) -> Int { return x + y }
print(f(3, 4))
```
→ `7`. Param annotations, return annotation all parse and are discarded. Exit 0.

### 11. Nested constructors + nested pattern match  ✅
```
type Tree { Node(Int, Tree, Tree); Leaf }
let t = Node(1, Node(2, Leaf, Leaf), Leaf)
match t { Node(v, l, r) -> { print(v); print("node") } ... }
```
→ `1` / `node`. Exit 0.

## Code Quality

### 12. Constructor detection via uppercase convention  ✅
Unregistered uppercase identifier treated as constructor:
```
let x = Foo       /* no `type Foo` declaration */
match x { Foo -> print("foo-matched") ... }
```
→ `foo-matched`. `is_constructor()` falls back to `is_upper_ident()` after registry miss. ✅

### 13. Representation consistency  ✅
```
let a = Solo          → prints: Solo
let b = Pair(1, 2)    → prints: (Pair 1 2)
```
- Nullary → quoted symbol → evaluates to **symbol** (`Solo`)
- N-ary → `(cons (quote Name) (cons arg ...))` → **proper list** (`(Pair 1 2)`)

Consistent with spec.

### 14. Type annotations fully discarded  ✅
The reader skips annotation tokens by advancing `lx->pos` past them at parse time (no AST node emitted):
```c
if (lx->src[lx->pos] == ':') {
    lx->pos++;
    while (... != ',' && ... != ')') lx->pos++;   /* discarded */
}
```
Same for `let x : Type = ...`, fn params, and `-> Type`. No runtime bytecode emitted for types. `f(3,4)` → `7` confirms no overhead.

## Minor Observations (non-blocking)

- Indentation drift on a few lines in both files (cosmetic, pre-existing style is already uneven).
- `constructors[256][64]` registry is module-global and never reset between `reader_ta_read` calls — fine for single-file programs; would matter only if multiple files share a VM in one process. Out of Phase 6a scope.
- `env_snapshot` unused-function warning pre-exists (not introduced here).

None affect acceptance.

## OVERALL: PASS

14/14 criteria met. Pure sugar implementation (zero changes to vm/gc/val/reader/main), clean regression, all behavioral scenarios correct, representation consistent, annotations truly compile-time.