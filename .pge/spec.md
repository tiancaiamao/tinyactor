# Spec: Phase 7 — Bootstrapping (Self-hosted Compiler)

## Architecture: Path B (ta → IR → bytecode)

```
         ┌──────────────────────────────────────────┐
         │          compiler.ta (self-hosted)         │
         │  ┌──────────┐    ┌──────────────────┐     │
hello.ta →│  │ parser   │──→│  codegen         │──→  │ hello.tabc
         │  │ (.ta)     │    │  (.ta)           │     │
         │  └──────────┘    └──────────────────┘     │
         │       ↓                  ↑                │
         │    hello.tir              │                │
         └──────────────────────────────────────────┘
                           │
                     VM 加载 .tabc 运行
```

## Pipeline

```
Bootstrap (now):     hello.ta → reader_ta.c(C) → compile.c(C) → bytecode → VM runs
Phase 7 parser:      hello.ta → parser.ta → hello.tir (S-expression text)
Phase 7 codegen:     hello.tir → codegen.ta → hello.tabc (binary bytecode file)
Phase 7 loader:      VM gains vm_load_tabc() to run .tabc files
Phase 7 bootstrap:   compiler.ta compiles itself → behavior equivalence verified
```

## Sub-phases

### Phase 7a: C Helper Modules (file I/O + string ops + byte buffer)

Add C modules that compiler.ta will need:

```
// file module (C)
file.read(path: String) -> String        // read entire file
file.write(path: String, data: String)   // write string to file
file.exists(path: String) -> Int         // 1 if exists, 0 if not

// buf module (C) — mutable byte buffer for codegen
buf.new() -> Bytes                       // create empty buffer
buf.push_byte(buf: Bytes, b: Int)        // append one byte
buf.push_int32(buf: Bytes, n: Int)       // append 4 bytes big-endian
buf.push_int64(buf: Bytes, n: Int)       // append 8 bytes
buf.push_string(buf: Bytes, s: String)   // append raw string bytes
buf.write_to(buf: Bytes, path: String)   // write buffer to file
buf.length(buf: Bytes) -> Int            // get buffer size

// str module (C) — string utilities for tokenizer
str.char_at(s: String, i: Int) -> Int    // get char code at index (0 if out of range)
str.length(s: String) -> Int             // string length
str.substr(s: String, start: Int, len: Int) -> String  // substring
str.concat(a: String, b: String) -> String             // concatenate
str.to_int(s: String) -> Int            // parse string to integer
str.from_int(n: Int) -> String          // integer to string
str.eq(a: String, b: String) -> Int     // 1 if equal
```

Verification: each C function callable from .ta

### Phase 7b: .ta Tokenizer

Write `lib/tokenizer.ta`:
- `pub fn tokenize(src: String) -> Tokens`
- Where `Tokens` is a list of `(type, value)` pairs
- Token types: integer, string, symbol, keyword, lparen, rparen, lbrace, rbrace, comma, arrow, eq, op
- Uses `str.char_at`, `str.length`, `str.substr` from Phase 7a

Verification: tokenize hello.ta, compare token list against expected

### Phase 7c: .ta Parser (ta → .tir)

Write `lib/parser.ta`:
- `pub fn parse(tokens: Tokens) -> Forms`
- Produces pair-tree AST (same shapes as reader_ta.c)
- Output serialized as S-expression text (.tir format)

Verification: parse hello.ta → output .tir → C reader.c reads .tir → compile → same bytecode

### Phase 7d: .tabc File Format + VM Loader

Define binary format:
```
Header (16 bytes):
  magic:     "TABC" (4 bytes)
  version:   uint32 (1)
  n_symbols: uint32
  n_fns:     uint32

Symbol Table:
  for each symbol:
    length: uint32
    data:   bytes[length]

Function Table:
  for each fn_id:
    entry_offset: uint32 (offset into Code section)

Code Section:
  raw bytecode bytes
```

Add `vm_load_tabc(vm, path)` in api.c:
- Read .tabc file
- Parse header → symbols, fn_table
- Load code bytes into vm->code
- Find top_fn_id (last fn in table)

Verification: C compiler outputs .tabc, VM loads .tabc, runs correctly

### Phase 7e: .ta Codegen (.tir → .tabc)

Write `lib/codegen.ta`:
- `pub fn compile(tir: String) -> Bytes`
- Reads S-expression text, produces .tabc binary
- Uses `buf.*` module from Phase 7a
- Must produce semantically equivalent bytecode to compile.c

Verification: compile hello.tir → hello.tabc → VM runs → same behavior as C path

### Phase 7f: Bootstrap Verification

1. Use C compiler to compile compiler.ta (parser+codegen) → compiler.tabc
2. VM loads compiler.tabc, runs it on hello.ta → produces hello.tir + hello.tabc
3. Compare: VM runs C-compiled hello vs self-compiled hello → behavior identical
4. Self-host: compiler.tabc compiles compiler.ta again → compiler_v2.tabc
5. compiler_v2.tabc compiles hello.ta → same behavior as compiler_v1

## .tir Format

The .tir is S-expression text — exactly what reader.c already parses:

```
// hello.tir
(define (main) (print "hello"))

// server.tir
(define (server)
  (receive
    (((quote Ping) from) (send from (quote Pong)) (server))
    ((quote Stop) (print "done"))))
```

This means `.tir` files can be loaded directly via the existing `vm_load()` (Lisp reader path).

## .ta CLI Extension

```
tinyactor hello.ta          # current behavior: C compiler path
tinyactor hello.ta --emit-tir   # output hello.tir instead of running
tinyactor hello.ta --emit-tabc  # output hello.tabc instead of running
tinyactor hello.tabc        # run pre-compiled bytecode
tinyactor hello.tir         # run S-expression IR (via reader.c)
```

## Dependencies

```
7a (C modules) ← 7b (tokenizer)
7b ← 7c (parser)
7a + 7d (tabc format) ← 7e (codegen)
7c + 7e ← 7f (bootstrap)
```

## Acceptance Criteria (Phase 7 overall)

- [ ] compiler.ta (parser + codegen) compiles and runs under VM
- [ ] compiler.ta can compile hello.ta → hello.tabc
- [ ] VM loads hello.tabc and runs it correctly
- [ ] compiler.ta compiles itself → compiler.tabc
- [ ] compiler.tabc compiles hello.ta → same behavior
- [ ] All existing .lisp + .ta tests pass via C path (no regression)

## Out of Scope
- Optimizations (constant folding, dead code elimination)
- Error recovery in parser
- Multiple source files in one compilation unit (use module system)
- Debug info / source maps in .tabc