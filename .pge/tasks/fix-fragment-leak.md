# Task: Fix proc_die fragment memory leak

## Problem
`proc_die()` in `src/vm.c` (line 323) sets `PROC_DEAD`, decrements `active_procs`, closes `wait_fd`, and sends DOWN messages to watchers — but it **never frees the dying process's mailbox fragment list** (`p->mbox_frag_head`). These are `malloc`'d `MsgFragment` nodes that accumulate as a memory leak when actors die with unconsumed messages.

Design doc `.pge/phase4-design.md` §13 risk table explicitly calls for this: "proc_die 时释放 mailbox 所有 fragment".

## File
- `src/vm.c` — function `proc_die` at line 323

## Fix
Add fragment cleanup at the **end** of `proc_die` (after the DOWN-message delivery loop, since DOWN messages are built on `p`'s heap and delivered via `mbox_deliver` before we destroy `p`'s mailbox):

```c
/* Free all undelivered mailbox fragments */
pthread_mutex_lock(&p->mbox_lock);
MsgFragment *frag = p->mbox_frag_head;
while (frag) {
    MsgFragment *next = frag->next;
    free(frag);
    frag = next;
}
p->mbox_frag_head = p->mbox_frag_tail = NULL;
p->mbox_count = 0;
pthread_mutex_unlock(&p->mbox_lock);
```

The `mbox_lock` is required because another thread could be in `mbox_deliver()` trying to append to this process's mailbox concurrently.

## Constraints
- **Do NOT modify** `gc.c`, `reader.c`, `val.c`, `compile.c`, or `ta.h`
- Only touch `src/vm.c`
- Do not remove or reorder existing logic in `proc_die` (state change, fd close, DOWN messages)
- Keep `MsgFragment` type as-is

## Acceptance Criteria
1. `make clean && make` — 0 errors, 0 warnings
2. All 49 test/scripts/*.lisp pass under both ST and MT (NWORKERS=4)
3. `example/scripts/echo_test.lisp` — PASS
4. `example/scripts/concurrent_test.lisp` — ALL PASS
5. `test/scripts/recv-scan.lisp` — PASS
6. `test/scripts/multithread-basic.lisp` — PASS (ST + MT 20 runs)
7. HTTP server: all 4 routes respond correctly + 10 concurrent requests