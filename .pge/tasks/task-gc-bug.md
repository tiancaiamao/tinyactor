# Task: Fix GC Stack Corruption on Buffer Growth

## Context
TinyActor VM uses a semi-space copying GC. When the GC collector (`gc_collect` in `src/gc.c`) detects that `gc_to_size > stack_start`, it grows both `p->mem` and `p->gc_to` by doubling `p->mem_size`. This triggers a critical bug.

## The Bug
In `gc_collect()` (src/gc.c, around line 80-120):

1. Stack roots are scanned using `Val *stack = (Val*)(p->mem + p->mem_size)` — this is correct, using the ORIGINAL mem_size
2. Then growth happens: `p->mem_size = new_size` (doubled)
3. Then swap: `p->mem = p->gc_to; p->mem_size = old_mem_size` (the doubled value)
4. Then stack copy: `stack_start = p->mem_size + p->sp * sizeof(Val)` — uses DOUBLED mem_size
5. `memcpy(p->mem + stack_start, old_mem + stack_start, stack_bytes)` — WRONG! The stack data in `old_mem` is at `original_mem_size + p->sp * sizeof(Val)`, NOT at `doubled_mem_size + p->sp * sizeof(Val)`

Result: After GC with growth, stack data is read from the wrong offset → corrupted stack → segfault or silent failure.

This affects: gc-deep-list.lisp (build-list of 100 pairs crashes), fib.lisp (fib≥28 crashes), tail-call-deep.lisp (sum≥800000 silently fails).

## What to Fix
In `src/gc.c`, function `gc_collect`:
1. Save `int orig_mem_size = p->mem_size;` at the TOP of the function (before any modification)
2. In the stack-copy section at the end, use `orig_mem_size` for calculating the SOURCE offset in `old_mem`
3. Use the current `p->mem_size` (which is the doubled size after growth) for the DESTINATION offset in `p->mem`

The fix should look like:
```c
/* Stack data source offset uses ORIGINAL mem_size (where stack actually lives in old_mem) */
int src_stack_start = orig_mem_size + p->sp * (int)sizeof(Val);
int dst_stack_start = p->mem_size + p->sp * (int)sizeof(Val);
int stack_bytes = orig_mem_size - src_stack_start;
if (stack_bytes > 0) {
    memcpy(p->mem + dst_stack_start, old_mem + src_stack_start, stack_bytes);
}
```

## Files to Modify
- `/Users/genius/project/tinyactor/src/gc.c` — only the `gc_collect` function

## Verification
```bash
cd /Users/genius/project/tinyactor && make
./ta test/scripts/gc-deep-list.lisp   # should output expected number, not segfault
./ta test/scripts/fib.lisp             # should output 832040
./ta test/scripts/tail-call-deep.lisp  # should output expected
```

## Rules
1. READ BEFORE WRITE — Read gc_collect function fully before editing
2. BUILD MUST PASS — `make` must succeed after the change
3. Output DONE: src/gc.c when complete