# Spec: Phase 8 — Module System for .ta Files

## Goal

Enable `import` of `.ta` source modules — not just C modules. A `.ta` file can `import` another `.ta` file and call its public functions.

## Current State

- `import net` → compile-time no-op. C modules pre-registered via `vm_register_module()`
- Dotted names `net.read(fd)` resolved at compile time via `cfunc_count` linear scan
- No mechanism to load `.ta` source as a module
- All code must be in one file

## Design

### Module = File

```
// math.ta
pub fn abs(n) {
  if n < 0 { -n } else { n }
}

pub fn max(a, b) {
  if a > b { a } else { b }
}

fn helper(x) { x * 2 }  // private — not exported
```

```
// main.ta
import math

fn main() {
  print(math.abs(-42))   // 42
  print(math.max(3, 7))  // 7
}
```

### `pub` keyword

- `pub fn name(...)` — function is exported (visible to importers)
- `fn name(...)` — function is private (file-local only)
- Currently ALL functions are effectively `pub` (no visibility control)

### How it works — compile-time inlining

When `compile_all` encounters `(import "math")`:

1. Find `math.ta` in search path (same directory as importer, then `lib/`)
2. Read and parse the file using `reader_ta_read`
3. Compile its `pub fn` definitions into the current bytecode
4. Rename exported functions: `abs` → `math.abs`, `max` → `math.max`
5. Compile private functions normally (they exist but aren't accessible via dotted name)

This is **static inlining** — the module's code is compiled into the same bytecode blob. No runtime module loading.

### Function renaming

```
// math.ta compiled into main.ta's bytecode:
//   "abs"  → "math.abs"   (for pub fns, so dot calls resolve)
//   "max"  → "math.max"
//   "helper" → "math.__helper"  (private, mangled name)
```

When compiler sees `math.abs(-42)`:
- Symbol `math.abs` is looked up in cfunc table (C modules)
- If not found, looked up in fn_table (user functions)
- Since we renamed `abs` → `math.abs` during import compilation, it resolves

### Search path

1. Same directory as the importing file: `{importer_dir}/math.ta`
2. Standard library: `lib/math.ta`
3. (Future) custom paths via environment variable

### Implementation

#### 1. reader_ta.c: Parse `pub` keyword

```
pub fn name(params) { body }   →  (define (name params) body)
```

The `pub` is parsed and discarded — it's a marker for the import system, not the reader. The reader produces the same `(define ...)` AST.

BUT: we need to track which functions are `pub`. Options:
- Option A: Return a different AST form: `(pub (define (name params) body))`
- Option B: Return metadata alongside: `(define_pub (name params) body)`

**Chosen: Option B** — `define_pub` as a new head symbol.

```
pub fn abs(n) { ... }  →  (define_pub (abs n) ...)
fn helper(x) { ... }   →  (define (helper x) ...)
```

#### 2. compile.c: Handle `define_pub`

In `compile_all`'s function-scanning pass, register `define_pub` functions just like `define`.

In the body compilation, compile `define_pub` exactly like `define`.

#### 3. api.c: Import resolution

In `vm_load_ta()` (or a new `vm_load_ta_file()`):

When processing top-level forms, collect them. When encountering `(import "math")`:

1. Resolve path: `{dir}/math.ta`
2. Read file into string
3. Call `reader_ta_read` in a loop to get all forms
4. For each `(define_pub (name ...) ...)` form:
   - Rename: `(define_pub (abs ...)) → (define (math.abs ...))`
5. For each `(define (name ...) ...)` form (private):
   - Rename: `(define (helper ...)) → (define (math.__helper ...))`  
   - Actually, keep as-is. Private functions don't conflict because callers use dotted names.
   - Simpler: leave private functions with original names. If there's a name collision, it's the user's problem.
6. Prepend the module's forms to the current file's forms list
7. Remove the `(import "math")` form

#### 4. compile.c: Resolve dotted function calls

Currently, `cx_call` does:
1. Check inline ops (+, -, etc)
2. Check cfunc registry (net.read etc)
3. Check user fn_table
4. Fallback

We need to add a step: when `math.abs` is not found in cfunc table, check if it's in fn_table. This already works because `compile_all` registers all function names, and `comp_find_fn` does a name lookup.

So **no change needed** — the compiler already searches fn_table by name.

### Circular imports

Phase 8 does NOT support circular imports. `A imports B, B imports A` → error. Detected by tracking import stack.

### Nested imports

`A imports B, B imports C` → C's pub functions get inlined into B, which gets inlined into A. But C's functions keep their `C.` prefix, not `B.C.`. This means:

```
// c.ta
pub fn helper() { ... }

// b.ta  
import c
pub fn main_fn() { c.helper() }

// a.ta
import b
fn main() { b.main_fn() }
```

When compiling `a.ta`:
1. Import `b.ta` → get `b.main_fn` + inlined `c.helper`
2. `b.main_fn()` calls `c.helper()` — this is in the bytecode, already compiled

Wait — this means nested imports need careful handling. When we inline `b.ta` into `a.ta`, `b.ta` itself has `import c`. We need to recursively process imports.

**Simplified approach**: Process imports recursively. Each module's pub functions get `module.` prefix. Private functions keep original name. Nested imports are resolved transitively.

### What about type declarations?

```
// msg.ta
pub type Msg { Ping(Pid); Stop }

// server.ta
import msg
fn server() {
  match recv() {
    Ping(from) -> ...   // Ping needs to be known
  }
}
```

Type declarations need to be shared across modules. When importing `msg`, its constructors (`Ping`, `Stop`) must be registered in the importing file.

**Implementation**: When processing an import, also collect `type` declarations and register their constructors.

## File modifications

1. **src/reader_ta.c**: Parse `pub` keyword → produce `(define_pub ...)` AST
2. **src/compile.c**: Handle `define_pub` (same as `define`)
3. **src/api.c**: Import resolution — recursive module loading, function renaming
4. **lib/**: Create `lib/` directory for standard library `.ta` files

## Acceptance Criteria

### L1 — Structural
- [ ] `make clean && make` — 0 errors
- [ ] `pub` keyword parsed correctly
- [ ] `import` of `.ta` files works
- [ ] No changes to vm.c, gc.c, ta.h, val.c, reader.c

### L2 — Behavioral

**Test 1: Basic import**
```ta
// lib/math.ta
pub fn abs(n) { if n < 0 { -n } else { n } }
pub fn max(a, b) { if a > b { a } else { b } }

// test/scripts/module-basic.ta
import math
fn main() {
  print(math.abs(-42))
  print(math.max(3, 7))
  print("PASS")
}
```
Expected: `42` then `7` then `PASS`

**Test 2: Private function hidden**
```ta
// lib/secret.ta
pub fn visible() { print("visible") }
fn hidden() { print("hidden") }

// test/scripts/module-private.ta
import secret
fn main() {
  secret.visible()   // works
  print("PASS")
}
```
Expected: `visible` then `PASS` (hidden is callable internally but not via `secret.hidden`)

**Test 3: ADT sharing across modules**
```ta
// lib/msg.ta
pub type Msg { Ping(Pid); Pong; Stop }

// test/scripts/module-adt.ta
import msg
fn server() {
  match recv() {
    Ping(from) -> { send(from, Pong); server() }
    Stop -> print("done")
  }
}
fn main() {
  let pid = spawn(fn { server() })
  send(pid, Ping(self()))
  match recv() {
    Pong -> print("got-pong")
    _ -> print("fail")
  }
  send(pid, Stop)
  print("PASS")
}
```
Expected: `got-pong` then `done` then `PASS`

**Test 4: Regression**
- All 49 .lisp + 7 .ta pass

## Out of Scope
- Dynamic/runtime module loading
- Circular import detection (Phase 8c)
- Package manager / versioning
- Conditional compilation