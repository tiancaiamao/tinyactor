# vm.c — CORRECT Specification for Downward-Growing Stack

Read ONLY: /Users/genius/project/tinyactor/ta.h (for struct definitions)
Then write: /Users/genius/project/tinyactor/src/vm.c

## CRITICAL: Stack Direction
ta.h uses DOWNWARD-GROWING stack. proc_push does sp-- and writes at proc_stack(p)[sp].
proc_stack(p) returns (Val*)(mem + mem_size). So:
- sp=0: empty stack. First push: sp=-1, value at proc_stack(p)[-1]
- sp=-3: 3 items. TOS at proc_stack(p)[-3], second at [-2], first at [-1]
- proc_push(p, v): sp--, proc_stack(p)[sp] = v
- proc_pop(p): v = proc_stack(p)[sp], sp++, return v
- proc_peek(p, offset): proc_stack(p)[sp + offset] — peek(0)=TOS, peek(1)=second

## CALL Convention (MUST follow exactly)

### Frame layout (after CALL N):
```
Index (relative to fp):  Content
fp - 4 : caller_sp  (Val containing int of caller's sp before call pushes)
fp - 3 : old_fp      (Val containing int of caller's fp)
fp - 2 : ret_addr    (Val containing int of return pc)
fp - 1 : closure     (Val, the closure being called)
fp + 0 : arg0
fp + 1 : arg1
...
fp + N-1 : argN-1
fp + N ... : free space for locals (sp is at fp-4 or below)
```

### OP_CALL N handler:
```c
case OP_CALL: {
    int32_t nargs; memcpy(&nargs, &p->code[p->pc], 4); p->pc += 4;
    // Save closure and args from stack
    Val closure_val = proc_peek(p, nargs);  // closure is deepest
    Val args[256];
    for (int i = 0; i < nargs; i++)
        args[i] = proc_peek(p, nargs - 1 - i);  // args[0] from nearest TOS
    // Pop all N+1 items
    p->sp += nargs + 1;
    int caller_sp = p->sp;
    int ret_pc = p->pc;
    int old_fp = p->fp;
    // Push args in REVERSE order (so arg0 ends up at lowest address = fp+0)
    for (int i = nargs - 1; i >= 0; i--)
        proc_push(p, args[i]);
    // Push header (4 items, from closure down to caller_sp)
    proc_push(p, closure_val);          // fp-1
    proc_push(p, val_int(ret_pc));      // fp-2
    proc_push(p, val_int(old_fp));      // fp-3
    proc_push(p, val_int(caller_sp));   // fp-4
    // Set fp to point at arg0 (which is at caller_sp - nargs)
    p->fp = caller_sp - nargs;
    // Get closure and jump
    HeapClosure *clos = val_as_clos(closure_val);
    p->pc = p->fn_table[clos->entry];
    break;
}
```

### OP_RET handler:
```c
case OP_RET: {
    Val ret_val = proc_pop(p);
    int caller_sp = (int)val_get_int(proc_stack(p)[p->fp - 4]);
    int old_fp    = (int)val_get_int(proc_stack(p)[p->fp - 3]);
    int ret_addr  = (int)val_get_int(proc_stack(p)[p->fp - 2]);
    p->sp = caller_sp;
    p->fp = old_fp;
    p->pc = ret_addr;
    proc_push(p, ret_val);
    break;
}
```

### OP_TAIL_CALL N handler:
```c
case OP_TAIL_CALL: {
    int32_t nargs; memcpy(&nargs, &p->code[p->pc], 4); p->pc += 4;
    Val closure_val = proc_peek(p, nargs);
    Val args[256];
    for (int i = 0; i < nargs; i++)
        args[i] = proc_peek(p, nargs - 1 - i);
    // Get current frame's caller info
    int caller_sp = (int)val_get_int(proc_stack(p)[p->fp - 4]);
    int old_fp    = (int)val_get_int(proc_stack(p)[p->fp - 3]);
    int ret_pc    = (int)val_get_int(proc_stack(p)[p->fp - 2]);
    // Pop new closure + args
    p->sp += nargs + 1;
    // Restore caller's frame
    p->sp = caller_sp;
    p->fp = old_fp;
    // Push new call from caller's perspective (reuse same ret_pc, old_fp, caller_sp)
    int CS = p->sp;
    for (int i = nargs - 1; i >= 0; i--)
        proc_push(p, args[i]);
    proc_push(p, closure_val);
    proc_push(p, val_int(ret_pc));
    proc_push(p, val_int(old_fp));
    proc_push(p, val_int(CS));
    p->fp = CS - nargs;
    // Jump
    HeapClosure *clos = val_as_clos(closure_val);
    p->pc = p->fn_table[clos->entry];
    break;
}
```

## OP_LOAD / OP_STORE
```c
case OP_LOAD: {
    int32_t off; memcpy(&off, &p->code[p->pc], 4); p->pc += 4;
    proc_push(p, proc_stack(p)[p->fp + off]);
    break;
}
case OP_STORE: {
    int32_t off; memcpy(&off, &p->code[p->pc], 4); p->pc += 4;
    proc_stack(p)[p->fp + off] = proc_pop(p);
    break;
}
```

## OP_CLOSURE
```c
case OP_CLOSURE: {
    int32_t fn_id, nfree;
    memcpy(&fn_id, &p->code[p->pc], 4); p->pc += 4;
    memcpy(&nfree, &p->code[p->pc], 4); p->pc += 4;
    HeapClosure *clos = (HeapClosure *)proc_heap_alloc(p,
        sizeof(HeapClosure) + nfree * sizeof(Val));
    clos->hdr.type = HEAP_CLOS;
    clos->entry = fn_id;
    clos->nfree = nfree;
    for (int i = 0; i < nfree; i++) {
        int32_t off; memcpy(&off, &p->code[p->pc], 4); p->pc += 4;
        clos->free[i] = proc_stack(p)[p->fp + off];
    }
    // Create tagged Val for the closure
    Val v = ((Val)TAG_CLOS << 48) | (uint64_t)(uintptr_t)clos;
    proc_push(p, v);
    break;
}
```

Note: TAG_CLOS is defined in ta.h. Check the exact tag constant. The value encoding
should match how val.c creates closure values. Look at val.c for val_clos or similar.

## OP_SPAWN fn_id
```c
case OP_SPAWN: {
    int32_t fn_id; memcpy(&fn_id, &p->code[p->pc], 4); p->pc += 4;
    Proc *np = proc_new(vm);
    np->code = vm->code;
    np->fn_table = vm->fn_table;
    np->fn_count = vm->fn_count;
    np->pc = vm->fn_table[fn_id];
    // Set up minimal frame (0 args)
    proc_push(p->fp is not set yet, need initial frame)
    // Initial frame: push caller_sp=0, old_fp=0, ret_addr=-1, closure=nil
    proc_push(np, val_nil());     // closure (unused for top-level)
    proc_push(np, val_int(-1));   // ret_addr (halt sentinel)
    proc_push(np, val_int(0));    // old_fp
    proc_push(np, val_int(0));    // caller_sp (base of stack = 0)
    np->fp = 0; // fp = sp + 4? No. After 4 pushes, sp = -4. fp should point to args (none), so fp = -4 + 4 = 0? Let me recalculate.
    // After 4 pushes: sp = -4
    // fp should = sp + 4 = 0. So fp = 0 means first local slot at fp+0 = proc_stack(p)[0] which is past the frame.
    // Actually fp = -4 + 4 = 0, and the frame items are at fp-4=-4, fp-3=-3, fp-2=-2, fp-1=-1.
    // LOAD 0 → proc_stack(p)[0] which is uninitialized. But there are no params, so slot 0 is for first local.
    // sp = -4 means next push goes to sp=-5, which is below the frame. That's fine.
    np->fp = np->sp + 4;  // fp points just past the header items
    // Actually let me compute: after pushes, sp = -4. Frame header at fp-4..fp-1.
    // For no args: fp+0 = first local. fp-4 = sp = -4. So fp = -4 + 4 = 0. ✓
    np->fp = np->sp + 4;
    vm->procs[np->pid] = np;
    runq_enqueue(vm, np->pid);
    proc_push(p, val_pid(np->pid));
    break;
}
```

## OP_SPAWN_CLOS
Pop closure from stack, create process from it.
Copy closure's free vars to the new process's stack so they're accessible via LOAD.

```c
case OP_SPAWN_CLOS: {
    Val closure_val = proc_pop(p);
    HeapClosure *clos = val_as_clos(closure_val);
    Proc *np = proc_new(vm);
    np->code = p->code;
    np->fn_table = p->fn_table;
    np->fn_count = p->fn_count;
    np->pc = p->fn_table[clos->entry];
    // Push free vars as "args" for the new process
    for (int i = clos->nfree - 1; i >= 0; i--)
        proc_push(np, clos->free[i]);
    // Push frame header
    proc_push(np, closure_val);
    proc_push(np, val_int(-1));   // ret_addr
    proc_push(np, val_int(0));    // old_fp
    proc_push(np, val_int(0));    // caller_sp
    // fp = sp + 4 + nfree (point past header + free vars? No...)
    // After pushes: nfree items, then 4 header items. sp = -(nfree + 4)
    // fp should point to first free var (which acts like arg0)
    // fp = sp + 4 + nfree? Let me check:
    // sp = -(nfree + 4). fp = -(nfree + 4) + 4 + nfree = 0. fp = 0.
    // fp + 0 = proc_stack(p)[0] = past the frame = first local slot. But free vars are at fp - (4+nfree) through fp - 5.
    // Hmm, the free vars should be at fp+0 through fp+nfree-1 so LOAD i accesses them.
    // But with our convention, args are at fp+0..fp+nfree-1, and the header is below at fp-1..fp-4.
    // Let me recalculate: pushes were in order: freeN-1, ..., free0, closure, ret, old_fp, caller_sp
    // After all pushes: sp = -(nfree + 4)
    // Items: [caller_sp] at -(nfree+4), [old_fp] at -(nfree+3), [ret] at -(nfree+2),
    //         [closure] at -(nfree+1), [free0] at -(nfree), ..., [freeN-1] at -1
    // fp should point at free0: fp = -(nfree)
    // Check: fp - 4 = -(nfree+4) = caller_sp ✓
    //         fp - 1 = -(nfree+1) = closure ✓
    //         fp + 0 = -nfree = free0 ✓
    //         fp + nfree - 1 = -1 = freeN-1 ✓
    np->fp = -(clos->nfree);
    vm->procs[np->pid] = np;
    runq_enqueue(vm, np->pid);
    proc_push(p, val_pid(np->pid));
    break;
}
```

## Match Opcodes
Use a match_failed flag. In Proc, add: int match_failed (or use a local variable in vm_step).
Actually, since Proc is defined in ta.h and we can't modify it, use a local variable in vm_step.

```c
int match_failed = 0;

case OP_MATCH_INT: {
    int64_t val; memcpy(&val, &p->code[p->pc], 8); p->pc += 8;
    Val subj = proc_pop(p);
    match_failed = !(val_is_int(subj) && val_get_int(subj) == val);
    break;
}
case OP_MATCH_SYM: {
    uint32_t idx; memcpy(&idx, &p->code[p->pc], 4); p->pc += 4;
    Val subj = proc_pop(p);
    match_failed = !(val_is_symbol(subj) && val_get_symbol(subj) == idx);
    break;
}
case OP_MATCH_NIL: {
    Val subj = proc_pop(p);
    match_failed = !val_is_nil(subj);
    break;
}
case OP_MATCH_PAIR: {
    Val subj = proc_pop(p);
    if (val_is_pair(subj)) {
        // Push cdr first, then car, so car is on top (popped first by next STORE)
        proc_push(p, val_get_cdr(subj));
        proc_push(p, val_get_car(subj));
        match_failed = 0;
    } else {
        match_failed = 1;
    }
    break;
}
case OP_MATCH_JUMP: {
    int32_t addr; memcpy(&addr, &p->code[p->pc], 4); p->pc += 4;
    if (match_failed) {
        p->pc = addr;
        match_failed = 0;
    }
    break;
}
```

## Other Opcodes (straightforward)
- PUSH_NIL: proc_push(p, val_nil())
- PUSH_TRUE: proc_push(p, val_true())
- PUSH_FALSE: proc_push(p, val_false())
- PUSH_INT8: int8_t v = (int8_t)p->code[p->pc++]; proc_push(p, val_int(v))
- PUSH_INT: int64_t v; memcpy(&v, &p->code[p->pc], 8); p->pc += 8; proc_push(p, val_int(v))
- PUSH_SYM: uint32_t idx; memcpy(&idx, &p->code[p->pc], 4); p->pc += 4; proc_push(p, val_symbol(idx))
- POP: proc_pop(p) (discard)
- DUP: proc_push(p, proc_peek(p, 0))
- ADD: Val b=proc_pop(p), a=proc_pop(p); proc_push(p, val_int(val_get_int(a)+val_get_int(b)))
- SUB, MUL, DIV, MOD: same pattern. DIV/MOD by zero → proc_die
- EQ: Val b=proc_pop(p), a=proc_pop(p); proc_push(p, val_eq(a,b) ? val_true() : val_false())
  where val_eq checks: both int and equal, both nil, both symbol and same idx, both pid and same
- LT: Val b=proc_pop(p), a=proc_pop(p); push(val_get_int(a) < val_get_int(b) ? true : false)
- LE: same with <=
- CONS: Val cdr=proc_pop(p), car=proc_pop(p); push(val_pair(p, car, cdr))  — allocates on heap
- CAR: push(val_get_car(proc_pop(p)))
- CDR: push(val_get_cdr(proc_pop(p)))
- IS_NIL: push(val_is_nil(proc_pop(p)) ? val_true() : val_false())
- IS_PAIR, IS_INT, etc.: same pattern
- JUMP: int32_t addr; memcpy(&addr, &code[pc], 4); pc = addr
- JUMP_IF_FALSE: int32_t addr; memcpy(...); Val v=proc_pop(p); if val_is_nil(v) or v==val_false(): pc = addr
- SEND: Val msg=proc_pop(p), pid_v=proc_pop(p); Proc *t = vm->procs[(int)val_get_pid(pid_v)];
  if t && t->state != PROC_DEAD: mbox_push(t, val_deep_copy(t, msg))
- RECV: if p->mbox_count == 0: p->state = PROC_WAIT_RECV; return -1;
  else: proc_push(p, mbox_pop(p))
- SELF: proc_push(p, val_pid(p->pid))
- MONITOR: Val pid_v=proc_pop(p); Proc *t = vm->procs[(int)val_get_pid(pid_v)];
  add p->pid to t->watchers; int ref = ++vm->next_monitor_ref;
  proc_push(p, val_int(ref))
  Note: add next_monitor_ref field to VM struct? Or use pid as ref? For simplicity, use pid.
- PRINT: Val v=proc_pop(p); print_val(vm, v); printf("\n")
- HALT: proc_die(vm, p, val_nil()); return -1

## proc_die
```c
void proc_die(VM *vm, Proc *p, Val reason) {
    p->state = PROC_DEAD;
    for (int i = 0; i < p->watcher_count; i++) {
        int wid = p->watchers[i];
        Proc *w = vm->procs[wid];
        if (!w || w->state == PROC_DEAD) continue;
        // Build DOWN message: ('DOWN ref pid reason)
        // For simplicity, just send reason
        Val down_sym = val_symbol(/* index of "DOWN" in symbol table, or intern it */);
        Val msg = val_deep_copy(w, reason);
        mbox_push(w, msg);
        if (w->state == PROC_WAIT_RECV) {
            w->state = PROC_RUNNING;
            runq_enqueue(vm, wid);
        }
    }
}
```
For the DOWN message, construct: cons('DOWN, cons(ref, cons(pid_val, cons(reason, nil))))
You'll need to build this on the watcher's heap via val_deep_copy.

## proc_new
```c
Proc *proc_new(VM *vm) {
    Proc *p = calloc(1, sizeof(Proc));
    p->pid = vm->proc_count++;
    p->state = PROC_RUNNING;
    p->mem_size = 65536;  // 64KB initial
    p->mem = malloc(p->mem_size);
    p->heap_ptr = 0;
    p->sp = 0;  // empty stack
    p->fp = 0;
    p->pc = 0;
    p->mbox_cap = 16;
    p->mbox = malloc(16 * sizeof(Val));
    p->watcher_cap = 4;
    p->watchers = malloc(4 * sizeof(int));
    vm->procs[p->pid] = p;
    return p;
}
```

## vm_run (scheduler)
```c
#define MAX_REDUCTIONS 1000

void vm_run(VM *vm) {
    while (vm->runq_head != vm->runq_tail) {
        int pid = vm->runq[vm->runq_head % vm->runq_cap];
        vm->runq_head++;
        Proc *p = vm->procs[pid];
        if (!p || p->state != PROC_RUNNING) continue;
        for (int r = 0; r < MAX_REDUCTIONS; r++) {
            if (vm_step(vm, p) != 0) break;
        }
        if (p->state == PROC_RUNNING)
            runq_enqueue(vm, p->pid);
    }
}
```

## vm_step returns 0 on success, -1 to break out of reduction loop.

## Mailbox (FIFO with circular buffer)
```c
void mbox_push(Proc *p, Val msg) {
    if (p->mbox_count >= p->mbox_cap) {
        p->mbox_cap *= 2;
        p->mbox = realloc(p->mbox, p->mbox_cap * sizeof(Val));
    }
    p->mbox[p->mbox_tail % p->mbox_cap] = msg;
    p->mbox_tail++;
    p->mbox_count++;
}
Val mbox_pop(Proc *p) {
    Val msg = p->mbox[p->mbox_head % p->mbox_cap];
    p->mbox_head++;
    p->mbox_count--;
    return msg;
}
```

## runq_enqueue
```c
void runq_enqueue(VM *vm, int pid) {
    if (vm->runq_tail - vm->runq_head >= vm->runq_cap) {
        // grow
        int new_cap = vm->runq_cap * 2;
        int *new_q = malloc(new_cap * sizeof(int));
        for (int i = vm->runq_head; i < vm->runq_tail; i++)
            new_q[i - vm->runq_head] = vm->runq[i % vm->runq_cap];
        free(vm->runq);
        vm->runq = new_q;
        vm->runq_cap = new_cap;
        vm->runq_head = 0;
        int count = vm->runq_tail;
        vm->runq_tail = count;
    }
    vm->runq[vm->runq_tail % vm->runq_cap] = pid;
    vm->runq_tail++;
}
```

## print_val
```c
void print_val(VM *vm, Val v) {
    if (val_is_int(v)) printf("%lld", (long long)val_get_int(v));
    else if (val_is_nil(v)) printf("nil");
    else if (val_is_true(v)) printf("true");
    else if (v == val_false()) printf("false");
    else if (val_is_symbol(v)) printf("%s", vm->symbols[val_get_symbol(v)]);
    else if (val_is_string(v)) {
        HeapString *s = val_get_string(v);  // or val_as_string? check val.c
        printf("%.*s", s->len, s->data);
    }
    else if (val_is_pair(v)) {
        printf("(");
        print_val(vm, val_get_car(v));
        Val rest = val_get_cdr(v);
        while (val_is_pair(rest)) {
            printf(" ");
            print_val(vm, val_get_car(rest));
            rest = val_get_cdr(rest);
        }
        if (!val_is_nil(rest)) {
            printf(" . ");
            print_val(vm, rest);
        }
        printf(")");
    }
    else if (val_is_pid(v)) printf("<pid %d>", (int)val_get_pid(v));
    else printf("?");
}
```

## Notes
- val_deep_copy is in val.c, use it for SEND
- val_pair(p, car, cdr) allocates on process heap, use for CONS
- Check ta.h for exact function names (val_get_string vs val_as_string, etc.)
- HeapClosure has fields: hdr (HeapHeader), entry (int), nfree (int), free[] (Val[])
- proc_stack(p) returns Val* base, use for fp-relative access
- For proc_new, register proc in vm->procs array
- VM needs: proc_count, procs[65536], runq, runq_head, runq_tail, runq_cap
  next_monitor_ref. Check ta.h for what's already declared.