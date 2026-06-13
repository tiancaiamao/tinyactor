# vm.c — PRE-DIGESTED SPECIFICATION
# Read ONLY this file and ta.h. Do NOT read compile.c or design.md.

## Reference: ta.h structures
Read /Users/genius/project/tinyactor/ta.h for Proc, VM, HeapClosure, etc. field names.

## Stack Convention
- Stack grows UPWARD: push = stack[sp++], pop = return stack[--sp]
- fp (frame pointer) points to arg0's position
- Stack frame layout:
  stack[fp+3] = saved_caller_fp  (as val_int)
  stack[fp+2] = saved_ret_addr   (as val_int)
  stack[fp+1] = closure          (as val_clos)
  stack[fp+0] = arg0 / first local
  stack[fp+1] = arg1 / second local (if N>=2)
  ...
  sp points just past the last used slot

## Opcode Formats (exact byte layout)
Each opcode is 1 byte, followed by inline operands:

| Opcode | Operands |
|--------|----------|
| PUSH_NIL, PUSH_TRUE, PUSH_FALSE, POP, DUP, ADD, SUB, MUL, DIV, MOD, EQ, LT, LE, CONS, CAR, CDR, IS_NIL, IS_PAIR, IS_INT, IS_STRING, IS_BYTES, IS_PID, HALT, RET, RECV, SELF | none |
| PUSH_INT8 | i8: 1 byte (signed) |
| PUSH_INT | i64: 8 bytes (memcpy from code) |
| PUSH_SYM | idx: 4 bytes |
| LOAD | offset: 4 bytes |
| STORE | offset: 4 bytes |
| JUMP | addr: 4 bytes |
| JUMP_IF_FALSE | addr: 4 bytes |
| CLOSURE | fn_id: 4 bytes, nfree: 4 bytes, then nfree × (offset: 4 bytes) |
| CALL | nargs: 4 bytes |
| TAIL_CALL | nargs: 4 bytes |
| SPAWN | fn_id: 4 bytes |
| SPAWN_CLOS | none |
| SEND | none |
| MONITOR | none |
| PRINT | none |
| MATCH_INT | value: 8 bytes |
| MATCH_SYM | idx: 4 bytes |
| MATCH_NIL | none |
| MATCH_PAIR | none |
| MATCH_JUMP | addr: 4 bytes |

## Opcode Semantics

### Stack ops
- PUSH_NIL: push val_nil()
- PUSH_TRUE: push val_true()
- PUSH_FALSE: push val_false()
- PUSH_INT8: read i8, push val_int((int64_t)i8)
- PUSH_INT: read i64, push val_int(i64)
- PUSH_SYM: read idx(4B), push val_symbol(idx)
- LOAD off: push stack[fp + off]
- STORE off: stack[fp + off] = pop()
- POP: sp--
- DUP: push stack[sp-1]

### Arithmetic/Comparison
- ADD: b=pop(), a=pop(); push val_int(val_get_int(a)+val_get_int(b))
- SUB: b=pop(), a=pop(); push val_int(val_get_int(a)-val_get_int(b))
- MUL, DIV, MOD: same pattern. DIV/MOD by zero → proc_die
- EQ: b=pop(), a=pop(); push val_true if equal else val_false
  - equal when: both int and same value, both nil, both same symbol, both same pid
- LT: b=pop(), a=pop(); push val_true if val_get_int(a) < val_get_int(b)
- LE: same with <=

### Pair
- CONS: cdr=pop(), car=pop(); push val_pair(p, car, cdr)  (allocates on heap)
- CAR: v=pop(); push val_get_car(v)
- CDR: v=pop(); push val_get_cdr(v)

### Type checks
- IS_NIL: v=pop(); push val_is_nil(v) ? val_true() : val_false()
- IS_PAIR, IS_INT, IS_STRING, IS_BYTES, IS_PID: same pattern

### Control flow
- JUMP addr: pc = addr
- JUMP_IF_FALSE addr: v=pop(); if val_is_nil(v) or val_is_false(v): pc = addr

### Function call/return

**CALL nargs** — Before: stack has [closure] [arg0] ... [argN-1] <- sp
1. Save: old_fp = p->fp, ret_pc = p->pc (already past CALL+operands)
2. Pop all: closure = stack[--sp]; args[N] pop from sp
   Or: save closure=stack[sp-N-1], then sp -= (N+1)
3. Push frame header: push(val_int(old_fp)), push(val_int(ret_pc)), push(closure)
4. fp = sp (points past closure, at the first free slot)
5. Push args: for i=0..N-1: push(args[i])
6. After pushes: fp[0]=arg0, fp[1]=arg1, etc.
   fp[1]=closure, fp[2]=ret_pc, fp[3]=old_fp
7. Get HeapClosure from closure val
8. pc = vm->fn_table[closure->entry] (entry stores fn_id, look up in fn_table)

**RET** —
1. ret_val = pop()
2. caller_fp = (int)val_get_int(stack[fp+3])
3. ret_addr = (int)val_get_int(stack[fp+2])
4. sp = fp + 4  (restore caller's sp: past old_fp)
5. fp = caller_fp
6. pc = ret_addr
7. push(ret_val)

**TAIL_CALL nargs** —
1. Save new closure and args from stack (same as CALL step 2)
2. Get current frame's caller info:
   caller_fp = (int)val_get_int(stack[fp+3])
   caller_ret = (int)val_get_int(stack[fp+2])
3. sp = fp + 4  (clear current frame, restore caller's sp)
4. fp = caller_fp
5. Push frame header: push(val_int(caller_fp)), push(val_int(caller_ret)), push(closure)
6. fp = sp
7. Push args, set pc from closure

**CLOSURE fn_id nfree [offsets...]** —
1. Read fn_id (4B), nfree (4B)
2. Allocate HeapClosure on process heap: header + nfree * sizeof(Val)
3. closure->hdr.type = HEAP_CLOS
4. closure->entry = fn_id
5. closure->nfree = nfree
6. For i=0..nfree-1: read offset(4B); closure->free[i] = stack[fp + offset]
7. Push as tagged value: val from val.c that wraps the HeapClosure pointer

### Actor primitives
- SPAWN fn_id:
  1. Read fn_id(4B)
  2. Create new Proc (proc_new)
  3. Share code/fn_table/fn_count with VM
  4. Set pc = vm->fn_table[fn_id]
  5. Setup minimal frame: push(val_int(0)), push(val_int(-1)), push(val_nil())
     fp=3, sp=3 (so fp[0..2] are filled, sp is at 3)
  6. Register proc in vm->procs[pid]
  7. Add to run queue
  8. Push val_pid(new_pid) to caller's stack

- SPAWN_CLOS:
  1. Pop closure from stack
  2. Create new Proc
  3. Share code/fn_table
  4. Get HeapClosure, set pc = vm->fn_table[closure->entry]
  5. Push saved info: val_int(0), val_int(-1), closure
  6. fp = sp (= 3)
  7. Push free vars: for i=0..nfree: push(closure->free[i])
  8. sp = fp + nfree
  9. Register proc, add to runq
  10. Push val_pid(new_pid) to caller

- SEND: msg=pop(), pid_val=pop(); target=vm->procs[val_get_pid(pid_val)]
  if target alive: mbox_push(target, val_deep_copy(target, msg))

- RECV: if mailbox empty: state=WAIT_RECV, return -1
  else: push(mbox_pop(p)), return 0

- SELF: push(val_pid(p->pid))

- MONITOR: pid_val=pop(); target=vm->procs[val_get_pid(pid_val)]
  add p->pid to target->watchers
  ref = ++vm->next_ref
  push(val_int(ref))  (or use a ref tag)

### Match opcodes
All match ops work with a "match_failed" flag per process.
After each match op, MATCH_JUMP checks the flag.
MATCH opcodes ALWAYS pop the subject value (from the preceding LOAD).

- MATCH_INT value: subj=pop(); if val_is_int(subj) && val_get_int(subj)==value: clear flag; else: set flag
- MATCH_SYM idx: subj=pop(); if val_is_symbol(subj) && val_get_symbol(subj)==idx: clear flag; else: set flag
- MATCH_NIL: subj=pop(); if val_is_nil(subj): clear flag; else: set flag
- MATCH_PAIR: subj=pop(); if val_is_pair(subj): push(val_get_cdr(subj)), push(val_get_car(subj)), clear flag; else: set flag
  Note: push cdr first, then car, so car is on top. The next two STORE instructions will pop car then cdr.
- MATCH_JUMP addr: if match_failed flag is set: pc = addr; else: continue

### Print
- PRINT: v=pop(); call print_val(vm, v); printf("\n")

### Halt
- HALT: call proc_die(vm, p, val_nil()); return -1

## Process Death
void proc_die(VM *vm, Proc *p, Val reason):
1. p->state = PROC_DEAD
2. For each watcher in p->watchers:
   a. Build DOWN msg: cons(val_symbol(DOWN_sym_idx), cons(val_int(ref), cons(val_pid(p->pid), cons(reason, val_nil()))))
   b. Deep copy to watcher's heap
   c. mbox_push to watcher
   d. If watcher state == WAIT_RECV: set RUNNING, enqueue

## Scheduler
void vm_run(VM *vm):
  while runq not empty:
    pid = dequeue
    proc = vm->procs[pid]
    if !proc or state != RUNNING: continue
    for r = 0..999:
      if vm_step(vm, proc) != 0: break
    if state == RUNNING: enqueue(pid)

## Helper: Run queue
void runq_enqueue(VM *vm, int pid):
  vm->runq[vm->runq_tail % cap] = pid
  vm->runq_tail++
  Grow if full

## Helper: print_val
void print_val(VM *vm, Val v):
  if int: printf("%lld", val_get_int(v))
  if nil: printf("nil")
  if true: printf("true")
  if false: printf("false")
  if symbol: printf("%s", vm->symbols[val_get_symbol(v)])
  if string: printf("%.*s", len, data)
  if pair: printf("("); print car; loop cdr printing space+elem; if dotted print " . " + cdr; printf(")")
  if pid: printf("<pid %d>", val_get_pid(v))