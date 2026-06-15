# Spec: Phase 7e — Self-hosted Codegen (lib/codegen.ta)

## Goal
Write a codegen module in TinyActor's own .ta syntax that compiles AST (pair-tree from parser) into .tabc bytecode, completing the self-hosting pipeline: `.ta source → tokenizer → parser → AST → codegen → .tabc → VM`.

## Context
- The C compiler (`src/compile.c`, 1368 lines) is the reference implementation
- The codegen must produce byte-for-byte equivalent .tabc output to the C compiler
- Modules available: `buf` (mutable byte buffers), `str` (string operations), `file` (file I/O)
- The parser produces pair-tree AST in the same format as the C reader: `(define (name params...) body)`, `(if cond then else)`, `(let var expr body)`, `(begin e1 e2 ...)`, `(op left right)`, `(fn arg1 arg2 ...)`, etc.

## .tabc Format (little-endian)
```
Header:
  "TABC" (4 bytes)
  u32 version (= 1)
  u32 n_symbols
  u32 n_fns
  u32 top_fn_id
  u32 code_len

Symbol Table:
  for each symbol: u32 length, then raw bytes

Function Table:
  for each function: u32 code_offset

Code Section:
  raw bytecode
```

## Opcode Numbers (for emission)
```
0=PUSH_NIL  1=PUSH_TRUE  2=PUSH_FALSE
3=PUSH_INT8(i8)  4=PUSH_INT(i64=8bytes)  5=PUSH_SYM(u32 idx)
6=PUSH_STRING(u32 len + data)  7=LOAD(i32 off)  8=STORE(i32 off)
9=CONS  10=CAR  11=CDR
12=ADD  13=SUB  14=MUL  15=DIV  16=MOD
17=EQ  18=LT  19=LE
20=IS_NIL  21=IS_PAIR  22=IS_INT  23=IS_STRING  24=IS_BYTES  25=IS_PID
26=JUMP(i32 addr)  27=JUMP_IF_FALSE(i32 addr)
28=POP  29=DUP
30=CLOSURE(u32 fn_id, u8 nfree, [i32 offset...])
31=CALL(u8 nargs)  32=TAIL_CALL(u8 nargs)  33=RET
34=SPAWN(u32 fn_id)  35=SPAWN_CLOS
36=SEND  37=RECV  38=RECV_PEEK  39=RECV_COMMIT  40=SELF  41=MONITOR
42=PRINT  43=HALT
44=MATCH_INT(i64)  45=MATCH_SYM(u32)  46=MATCH_NIL  47=MATCH_PAIR  48=MATCH_JUMP(i32)
49=STR_LEN  50=STR_CONCAT  51=STR_SLICE  52=STR_EQ
53=CCALL(u32 cidx, u8 nargs)  54=ENTER(u32 nslots)
```

## Pre-registered Symbol Table (indices 0-42)
```
[0]=quote [1]=define [2]=lambda [3]=if [4]=begin [5]=let [6]=letrec
[7]=match [8]=spawn [9]=send [10]=recv [11]=self [12]=monitor
[13]=cons [14]=car [15]=cdr [16]=+ [17]=- [18]=* [19]=/ [20]=%
[21]== [22]=< [23]=<= [24]=> [25]=>= [26]=null? [27]=pair?
[28]=int? [29]=string? [30]=bytes? [31]=pid? [32]=print
[33]=true [34]=false [35]=DOWN [36]=nil [37]=_ [38]=and [39]=or
[40]=not [41]=set!
```
User symbols start at index 43+.

## Compilation Algorithm (mirrors compile.c)

### Two-pass approach:
**Pass 1**: Walk AST, register all `(define (name ...) ...)` functions → assign fn_ids
**Pass 2**: Emit bytecode for each function body, then top-level code

### Code layout:
```
JUMP <top_level_offset>     // Always first
[fn body 1] RET             // Each function's bytecode
[fn body 2] RET
...
[top-level code]            // Spawn main + HALT, or top expressions
PUSH_NIL HALT
```

### Function compilation:
1. Record entry offset for fn_id
2. Emit OP_ENTER with placeholder nslots
3. Set next_slot=-5, max_slots=-5 (frame header is 5 values: saved_fp, saved_pc, ret_addr, nargs, saved_recv_mark)
4. Build param environment: param[0]=offset 0, param[1]=offset 1, ...
5. Compile body with cx_expr/cx_body
6. Emit OP_RET
7. Patch OP_ENTER nslots = (-5) - max_slots

### Expression compilation (cx_expr):
- **Integer**: PUSH_INT8 (if fits in i8) or PUSH_INT
- **String**: PUSH_STRING (len + data)
- **nil**: PUSH_NIL; **true**: PUSH_TRUE; **false**: PUSH_FALSE
- **Symbol** (variable ref): LOAD with env slot offset
- **(quote sym)**: PUSH_SYM with symbol table index
- **(if cond then else)**: compile cond → JUMP_IF_FALSE patch → compile then → JUMP patch → compile else → patch both
- **(let var expr body)**: compile expr → STORE slot → extend env → compile body
- **(begin e1 e2 ...)**: compile each expr; POP all but last; tail position for last
- **(op left right)**: inline ops (e.g., + → OP_ADD); or CALL if not inline
- **(fn args...)**: push args → CALL nargs (or TAIL_CALL if in tail position)
- **(lambda params body)**: CLOSURE with free variable capture
- **(match scrut arms...)**: pattern matching with MATCH_* opcodes and backpatching
- **(spawn lam)**: SPAWN with fn_id
- **(send pid msg)**: compile pid → compile msg → SEND
- **(recv arms...)**: RECV/RECV_PEEK/RECV_COMMIT pattern

### Inline operators (symbol → opcode):
```
+ → ADD(12)  - → SUB(13)  * → MUL(14)  / → DIV(15)  % → MOD(16)
= → EQ(17)  < → LT(18)  <= → LE(19)
cons → CONS(9)  car → CAR(10)  cdr → CDR(11)
null? → IS_NIL(20)  pair? → IS_PAIR(21)  int? → IS_INT(22)
print → PRINT(42)  self → SELF(40)
```

### C function calls:
If function name is not an inline op and not a user-defined function, emit CCALL with the C module function index.

## Acceptance Criteria

### L1 — Structural
- [ ] `lib/codegen.ta` exists and compiles without error — Verify: `./tinyactor -c lib/codegen.ta` (no crash)
- [ ] Codegen module exports `compile(ast)` function — Verify: `grep 'pub fn compile' lib/codegen.ta`
- [ ] Codegen module exports `compile_file(src_path, out_path)` function — Verify: `grep 'pub fn compile_file' lib/codegen.ta`

### L2 — Behavioral
- [ ] Compiles `"fn main() { print(\"hello\") }"` to valid .tabc — Verify: `./tinyactor codegen_test.ta` where codegen_test imports codegen, compiles source, writes .tabc, then compare with C compiler output
- [ ] The generated .tabc runs correctly in the VM — Verify: `./tinyactor output.tabc` prints "hello"
- [ ] Compiles `"fn add(x,y) { x + y } fn main() { print(add(3,4)) }"` — Verify: generated .tabc prints "7"
- [ ] Compiles let/if: `"fn main() { let x = 42 if x > 0 { print(\"pos\") } }"` — Verify: generated .tabc prints "pos"
- [ ] All existing regression tests still pass — Verify: `for f in test/scripts/*.ta; do ./tinyactor "$f"; done`

## Constraints
- Must work within TinyActor's language features (no new C code)
- Use buf module for bytecode emission: `buf.new()`, `buf.push_byte()`, `buf.push_int32()`, `buf.push_int64()`, `buf.push_string()`, `buf.write_to()`
- Use str module for string operations: `str.eq()`, `str.to_sym()`, `str.char_at()`
- Threaded-state pattern (like parser): pass state through function returns
- Code must be efficient enough to not trigger GC issues with small programs

## Out of Scope (Phase 7e-2 and beyond)
- Closures/free variable capture (lambda with captured vars)
- Match/pattern compilation
- spawn/send/receive compilation
- Module imports
- Type declarations
- C function calls (CCALL)
- Exhaustiveness checking