# Spec: Phase 6a — ADT (Algebraic Data Types) + Constructor Syntax

## Goal

Add `type` declarations and constructor syntax to the `.ta` language, eliminating the need to hand-build messages with `cons` chains. ADT values are pure syntactic sugar over existing pair trees — **zero VM/gc changes**.

## Core Design

### Runtime Representation (zero new runtime types)

```
// Nullary constructor: value is just a symbol
Bye             → runtime: val_symbol("Bye")
                → reader AST: (quote Bye)

// N-ary constructor: value is a proper list with symbol head
Hello(pid, "hi") → runtime: ('Hello <pid> "hi")
                 → reader AST: (cons (quote Hello) (cons <pid-expr> (cons "hi" nil)))
```

This is exactly what we currently build manually:
```
// Before (painful):
cons('Hello, cons(self(), cons("hi", nil)))

// After (clean):
Hello(self(), "hi")
```

### Constructor Convention

**Capitalized identifier = constructor.** Lowercase = function/variable.
This is the Haskell/Gleam convention and eliminates the need for a two-pass parser.

```
Hello(from, text)    // Constructor call (capital H)
handle_client(fd)    // Function call (lowercase h)
```

### Pattern Matching (also pure sugar)

```
match msg {
  Hello(from, text) -> print(text)    // matches ('Hello <pid> <str>)
  Bye -> print("bye")                  // matches symbol Bye
}
```

Translates to existing pattern forms:
```
Hello(from, text)  →  ['Hello, from, text]   (list pattern with quoted head)
Bye                →  'Bye                   (quoted symbol pattern)
```

## Grammar Additions

### Type declaration (top-level)

```
typeDecl ::= 'type' typeName '{' variantDecl* '}'

typeName  ::= UpperIdent      // Capitalized

variantDecl ::=
    upperIdent                // nullary: Red, Green, Bye
  | upperIdent '(' typeList? ')'  // n-ary: Hello(Pid, String)

typeList   ::= typeName (',' typeName)*
```

Example:
```
type Msg {
  Hello(Pid, String)
  Bye
}

type Result {
  Ok(String)
  Err(String)
}

type Color {
  Red
  Green
  Blue
}
```

The reader collects constructor names but produces a no-op AST form `(type)` for the compiler to skip.

### Type annotations (parsed and discarded in Phase 6a)

```
// Function with annotations
fn add(x: Int, y: Int) -> Int {
  x + y
}

// Let with annotation
let x: String = net.read(fd)
```

The `: Type` and `-> Type` parts are parsed by the reader but discarded. The AST produced is identical to the un-annotated version:
```
fn add(x: Int, y: Int) -> Int { x + y }
→ (define (add x y) (+ x y))
```

### Constructor expressions

```
// Nullary constructor
Bye              → (quote Bye)

// N-ary constructor  
Hello(self(), "hi")
→ (cons (quote Hello) (cons (self) (cons "hi" nil)))

// Nested constructors
Ok(Err("timeout"))
→ (cons (quote Ok) (cons (cons (quote Err) (cons "timeout" nil)) nil))
```

### Constructor patterns

```
// Nullary in pattern
Bye              → (quote Bye)     [quoted symbol pattern]

// N-ary in pattern
Hello(from, text) → proper list pattern:
  [(quote Hello), from, text]
  = val_pair(quote_hello, val_pair(sym("from"), val_pair(sym("text"), nil)))

// With wildcard
Hello(_, text)   → [(quote Hello), _, text]

// Nested
Ok(Err(msg))     → [(quote Ok), [(quote Err), msg]]
```

## Implementation Plan

### File: src/reader_ta.c (primary changes)

1. **Constructor table**: static array of constructor names, populated by `type` declarations

2. **`is_constructor(char *name, int len)`**: returns 1 if name is in the constructor table OR starts with uppercase (convention-based fallback)

3. **`parse_toplevel`**: add `type` keyword handling
   - Parse `type Name { Variant1(Type1, Type2); Variant2; ... }`
   - Register each variant name in constructor table
   - Return `(type)` form (symbol "type" as a pair, treated as no-op by compiler)

4. **`parse_expr` / `parse_operand`**: when encountering a capitalized identifier:
   - If followed by `(`: parse as constructor call
     - `Foo(a, b)` → `(cons (quote Foo) (cons a_expr (cons b_expr nil)))`
   - If not followed by `(`: nullary constructor
     - `Foo` → `(quote Foo)`

5. **`parse_pattern`**: when encountering a capitalized identifier:
   - If followed by `(`: parse as constructor pattern
     - `Foo(a, b)` → proper list: `[(quote Foo), a_pat, b_pat]`
   - If not followed by `(`: nullary constructor pattern
     - `Foo` → `(quote Foo)` (quoted symbol pattern)

6. **Type annotation parsing**: in function params and let bindings:
   - `fn f(x: Int, y: String) -> Bool { }`
   - Parse `name`, then if `:` follows, skip the type name
   - Parse `-> ReturnType` after `)` in fn signature, skip it
   - `let x: Type = expr` → parse name, skip `: Type`, continue normally

### File: src/compile.c (minimal change)

Add one case to skip `(type)` top-level forms:

```c
// In cx_expr, before the general function call path:
if (sym_eq(c->vm, head, "type")) {
    emit_byte(&c->code, OP_PUSH_NIL);
    return;
}
```

Also in `compile_all`, skip type forms in the define-scanning pass (they're not defines, so they'll naturally be skipped — but verify).

### Files NOT changed

- `ta.h` — no new types needed
- `vm.c` — ADT values are just pair trees and symbols
- `gc.c` — no new memory patterns
- `val.c` — no new value types
- `reader.c` — Lisp reader unchanged

## Acceptance Criteria

### L1 — Structural
- [ ] `make clean && make` — 0 errors
- [ ] Type declarations parse without crash
- [ ] Constructor expressions compile and run
- [ ] Constructor patterns match correctly
- [ ] Type annotations parsed and discarded (no crash)
- [ ] All 49 .lisp tests still pass (zero regression)
- [ ] All 5 existing .ta tests still pass

### L2 — Behavioral

**Test 1: Basic ADT**
```ta
type Color { Red; Green; Blue }

fn main() {
  let c = Red
  match c {
    Red -> print("red")
    Green -> print("green")
    Blue -> print("blue")
  }
}
```
Expected output: `red`

**Test 2: N-ary constructors**
```ta
type Msg { Hello(Pid, String); Bye }

fn main() {
  let msg = Hello(self(), "world")
  match msg {
    Hello(from, text) -> {
      print("hello from")
      print(text)
    }
    Bye -> print("bye")
  }
}
```
Expected: `hello from` then `world`

**Test 3: Actor message passing with ADT**
```ta
type Request { Ping(Pid); GetStatus(Pid); Stop }
type Response { Pong; Status(String); Stopped }

fn server() {
  match recv() {
    Ping(from) -> {
      send(from, Pong)
      server()
    }
    GetStatus(from) -> {
      send(from, Status("running"))
      server()
    }
    Stop -> print("server-stopped")
  }
}

fn main() {
  let pid = spawn(fn { server() })
  send(pid, Ping(self()))
  match recv() {
    Pong -> print("got-pong")
    _ -> print("fail")
  }
  send(pid, GetStatus(self()))
  match recv() {
    Status(s) -> print(s)
    _ -> print("fail")
  }
  send(pid, Stop)
  print("PASS")
}
```
Expected: `got-pong` then `running` then `server-stopped` then `PASS`

**Test 4: Type annotations**
```ta
fn add(x: Int, y: Int) -> Int {
  x + y
}

fn main() {
  let result: Int = add(3, 4)
  print(result)
}
```
Expected: `7`

**Test 5: Nested constructors**
```ta
type Tree { Node(Int, Tree, Tree); Leaf }

fn main() {
  let t = Node(1, Node(2, Leaf, Leaf), Node(3, Leaf, Leaf))
  match t {
    Node(v, Node(v2, _, _), _) -> {
      print(v)
      print(v2)
    }
    Leaf -> print("leaf")
  }
}
```
Expected: `1` then `2`

## Out of Scope (Phase 6b+)
- Type checking / enforcement (annotations are documentation only)
- Exhaustiveness checking
- Type inference
- Generic types (`type Result(a, e)`)
- `pub` visibility