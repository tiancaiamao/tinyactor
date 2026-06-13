# Task 1: Fix Preempt Bug — Root Process Exit Must Terminate VM

## Problem
`test/scripts/preempt.lisp` outputs "ok" correctly but the VM never exits (timeout, exit code 124).

The root cause is in `vm_run()` in `src/vm.c`. The current loop:
```c
while (vm->rq_head != vm->rq_tail) {
    // dequeue, run, if still RUNNING → re-enqueue
}
```

When a process spawns a child that loops forever (e.g., `(define (loop) (loop))`), that child process never dies, gets re-enqueued forever, and `vm_run` never returns.

## What "Preempt" Test Does
```lisp
(define (loop) (loop))
(spawn 'loop)
(print "ok")
```
The main process spawns an infinite-loop child then prints "ok" and exits. The child should be cleaned up when the main/root process finishes.

## Required Fix
Modify `vm_run()` so that when the root process (the initial process created by `vm_run` / `vm_spawn` from `main.c`) terminates, all remaining processes are killed and the scheduler exits.

### Approach Options (pick whichever is cleanest):
1. **Track root process**: `vm_run` records the PID of the first process it runs. When that PID's process transitions to PROC_DEAD, kill all remaining processes and exit the loop.
2. **Ref-count running processes**: When process count drops to 0 (or only infinite-loop processes remain with no root), exit.
3. **Simplest**: After each scheduler tick, check if the root process (pid 0 or the first spawned process) is dead. If so, kill all others and return.

### Important Constraints:
- Do NOT break multi-process scenarios like ping_pong.lisp where ALL processes should run to completion
- The fix should only apply when the "root" / "entry" process dies — other processes dying should not kill siblings (that's what monitor is for)
- The root process is the one created by `vm_spawn` called from `main.c` or `vm_eval`

## Files to Modify
- `src/vm.c`: `vm_run()` function (~20 lines, around line 196)
- Possibly `ta.h` if you need to track root_pid in the VM struct

## Verification
```bash
cd /Users/genius/project/tinyactor
make clean && make

# Must: output "ok" AND exit code 0 (not 124 timeout)
timeout 5 ./tinyactor test/scripts/preempt.lisp
echo "exit code: $?"

# Must NOT break: these all still pass
./tinyactor test/scripts/ping_pong.lisp
./tinyactor test/scripts/many_actors.lisp
./tinyactor test/scripts/fib.lisp
./tinyactor test/scripts/tailcall.lisp
./tinyactor test/scripts/actor-ping-pong-stress.lisp
./tinyactor test/scripts/gc-multi-process-stress.lisp
./tinyactor test/scripts/monitor_test.lisp

# Full regression: must still be 42 pass
pass=0; fail=0; for f in test/scripts/*.lisp; do name=$(basename "$f"); out=$(timeout 5 ./tinyactor "$f" 2>&1); rc=$?; if [ $rc -eq 0 ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL($rc) $name: $out"; fi; done; echo "PASS: $pass  FAIL: $fail"
```

## Done Criteria
- preempt.lisp exits with code 0 and outputs "ok"
- All 42 previously-passing tests still pass
- Build is clean (make succeeds)