# Task: Fix 3 Remaining Bugs in TinyActor

## Project: /Users/genius/project/tinyactor/
Build: `cd /Users/genius/project/tinyactor && make clean && make`

## Bug 1: Closure free variable capture (closure.lisp outputs 3, should be 8)

**Test**: `./tinyactor test/scripts/closure.lisp`
**Expected**: `8`
**Actual**: `3`

**Cause hint**: `(define (adder n) (lambda (x) (+ x n)))` — the lambda inside adder should capture `n` as a free variable. When `((adder 5) 3)` is called, it should compute `(+ 3 5) = 8`, but it computes `(+ 3 0)` or `(+ 3 x)` instead, suggesting `n` is captured with wrong value or not captured at all.

**Files to investigate**: 
- `src/compile.c` — look at lambda compilation (cx_expr "lambda" case ~line 807), specifically how free variables are collected (cx_collect_free) and how OP_CLOSURE emits the captured slots
- `src/vm.c` — look at OP_CLOSURE handler (~line 369) to verify it correctly reads captured values from stack[fp + offset]
- `ta.h` — HeapClosure structure

The key question: when `(adder 5)` runs, `n` is at some stack slot in adder's frame. The lambda creates a closure capturing that slot. When the closure is later called with x=3, the captured `n` should still be 5. Trace the slot number and verify the closure actually stores it.

## Bug 2: print does not support strings (tailcall.lisp outputs nil, should output "done")

**Test**: `./tinyactor test/scripts/tailcall.lisp`  
**Expected**: `done`
**Actual**: `nil`

**Cause hint**: `(print "done")` — the reader parses `"done"` as a string, but the print_val function in vm.c doesn't handle strings properly, or the string isn't being compiled/pushed correctly.

**Files to investigate**:
- `src/vm.c` — find print_val or the OP_PRINT handler. Check if it handles val_is_string() case.
- `src/compile.c` — check how `(print "done")` is compiled. The `print` form compiles to OP_PRINT which pops and prints a value. But "done" as a string literal — does the compiler emit the right push instruction for it? Check the quote/literal handling in cx_expr.
- `src/reader.c` — verify string literals are parsed correctly.

## Bug 3: ping_pong.lisp segfaults

**Test**: `./tinyactor test/scripts/ping_pong.lisp`
**Expected**: `ping done\npong done\nall done`
**Actual**: segfault

This is the most complex test. It uses spawn, send, recv, match, monitor, and recursive actor message passing.

**Files to investigate**:
- `src/vm.c` — OP_SPAWN, OP_SEND, OP_RECV, OP_MATCH_PAIR, OP_MONITOR, and the scheduler
- After fixing Bug 1 (closures with free vars), ping_pong may work better since it uses closures with `(spawn (lambda () (ping 100 pong-pid)))` which captures `pong-pid`
- Also check: `(spawn 'pong)` — this should look up function "pong" by name and spawn it. Verify OP_SPAWN reads fn_id correctly.

**Note**: Bug 1 fix may partially fix Bug 3, since ping_pong uses closures that capture variables. Fix Bug 1 first, then re-test.

## Instructions
1. Read the relevant source files (ta.h, compile.c, vm.c, reader.c)
2. Fix Bug 1 first, verify closure.lisp outputs 8
3. Fix Bug 2, verify tailcall.lisp outputs "done"
4. Re-test ping_pong.lisp (may be fixed by Bug 1). If still crashes, debug and fix.
5. Run ALL test scripts at the end:
   ```bash
   for f in test/scripts/*.lisp; do echo "=== $f ===" && ./tinyactor "$f" 2>&1; echo "Exit: $?"; done
   ```
6. Remove any debug/trace code you added.
7. Output DONE when all tests pass.