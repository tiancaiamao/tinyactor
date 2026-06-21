# Task: Fix Stall Counter + Undefined Function Crash + Test Framework

## Context
TinyActor VM has 3 bugs in `src/vm.c` and 1 bug in `test/run_all_tests.sh`.

## Bug 1: Stall Counter Kills Long Computations (vm.c)

In `worker_loop()` (around line 480-530 of src/vm.c), the `stall` counter increments EVERY TIME a process finishes a 1000-reduction turn without changing state. For a single long-running computation:
- Each turn runs 1000 opcodes, then re-enqueues the process
- `stall++` every turn because state stays PROC_RUNNING
- After `stall > 10000` (10 million opcodes), ALL processes are killed

fib(28) needs ~15M opcodes → exceeds limit → killed silently.
fib(27) needs ~9.5M opcodes → barely fits.
tail-call-deep with sum(0,800000) needs ~8M opcodes — should fit but may not depending on opcode count per iteration.

### Fix
The stall counter should detect ACTUAL stalls (runq empty, no progress), NOT long-running computations. Options:
- **Option A**: Only increment `stall` when `ran == 0` (no process ran in the entire inner loop iteration), reset to 0 when `ran == 1`
- **Option B**: Remove the stall counter entirely for single-thread mode and rely on the `nfds == 0` check to terminate

**Recommended: Option A** — Keep the stall counter but only count when NOTHING ran. This preserves deadlock detection while not killing long computations.

## Bug 2: Undefined Function Call Causes Segfault (vm.c)

In the compiler (`src/compile.c`, around line 773), when `comp_find_fn` returns -1 (function not found), the compiler emits `OP_PUSH_NIL`. Then in the VM (`src/vm.c`), `OP_CALL` handler:
1. Gets `closure_val` from stack (which is nil)
2. Checks `(closure_val >> 48) == TAG_CLOS` — fails for nil
3. Falls to else branch: `HeapClosure *clos = val_as_clos(closure_val);` — casts nil to pointer
4. `p->pc = p->fn_table[clos->entry];` — dereferences nil → SEGFAULT

Same issue exists in `OP_TAIL_CALL`.

### Fix
In `src/vm.c`, OP_CALL and OP_TAIL_CALL handlers, add a check BEFORE using the closure:
```c
if ((closure_val >> 48) != TAG_CLOS && (closure_val >> 48) != TAG_CLOS_ID) {
    fprintf(stderr, "error: cannot call non-function value\n");
    p->state = PROC_DEAD;
    break;  // or return 1 from vm_step
}
```

Also fix the same pattern in OP_TAIL_CALL.

## Bug 3: Test Framework Masks Failures (run_all_tests.sh)

In `test/run_all_tests.sh`, the test runner treats exit codes 139 (segfault) and 124 (timeout) as PASS. Also, tests with empty output are counted as PASS.

### Fix
1. Only exit code 0 should be considered for PASS
2. Even with exit 0, check that output contains expected PASS token or is non-empty
3. Exit 139 (segfault) → FAIL with "SEGFAULT"
4. Exit 124 (timeout) → FAIL with "TIMEOUT"
5. Exit 0 but empty output → FAIL with "NO OUTPUT"

Read the current run_all_tests.sh to understand its structure before modifying.

## Files to Modify
- `/Users/genius/project/tinyactor/src/vm.c` — stall counter + OP_CALL/OP_TAIL_CALL nil check
- `/Users/genius/project/tinyactor/test/run_all_tests.sh` — test framework

## Verification
```bash
cd /Users/genius/project/tinyactor && make
./ta test/scripts/fib.lisp             # should output 832040 (not killed by stall)
./ta test/scripts/tail-call-deep.lisp  # should output expected (not killed)
./ta test/scripts/module-basic.ta      # should exit non-zero, non-139 (error, not crash)
./ta test/scripts/bytes-basic.lisp     # should not segfault (exit ≠ 139)
cd test && bash run_all_tests.sh       # segfaults/timeouts show as FAIL
```

## Rules
1. READ BEFORE WRITE — Read each function fully before editing
2. BUILD MUST PASS — `make` must succeed after changes
3. Output DONE: <file list> when complete