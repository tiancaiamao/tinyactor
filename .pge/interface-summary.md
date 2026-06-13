# TinyActor Runtime Interface Summary
# Generated for VM implementation — read this instead of analyzing source files

## 1. Proc Structure (from ta.h)
Fields used by VM:
- pid: int (auto-assigned)
- state: ProcState (PROC_RUNNING, PROC_WAIT_RECV, PROC_DEAD)
- pc: int (bytecode program counter)
- sp: int (stack pointer, starts at 0, grows UP: push = sp++)
- fp: int (frame pointer, points to arg0 in current frame)
- code: uint8_t* (shared bytecode, read-only reference)
- fn_table: int* (shared function table, fn_table[fn_id] = bytecode offset)
- fn_count: int
- stack: Val* (stack array, allocated separately)
- stack_cap: int
- mem: uint8_t* (heap for pair/string/etc allocation)
- mem_size: int
- heap_ptr: int (grows UP from 0)
- mbox: Val* (mailbox array)
- mbox_len, mbox_cap: int
- mbox_head: int (FIFO read position)
- watchers: int* (pids watching this process via monitor)
- watcher_count, watcher_cap: int
- reductions: int

## 2. VM Structure (from ta.h)
- procs: Proc** (indexed by pid, max 65536)
- proc_count: int
- code: uint8_t* (compiled bytecode)
- fn_table: int* 
- fn_count: int
- fn_names: char** (fn_names[fn_id] = function name)
- fn_name_count: int
- symbols: char** (symbol string table)
- sym_count: int
- runq: int* (run queue of pids)
- runq_head, runq_tail, runq_cap: int
- cfuncs: registered C functions (Phase 1: not needed, all inline opcodes)

## 3. CALL Convention (from compile.c)

### Stack grows UPWARD: push = stack[sp++] = val; pop = return stack[--sp]

### Before OP_CALL N:
Stack: ... [closure] [arg0] [arg1] ... [argN-1] ← sp
closure is at stack[sp - N - 1]
args are at stack[sp - N] through stack[sp - 1]

### OP_CALL N handler:
1. Closure heap_closure = val_get_clos(stack[sp - N - 1])
2. Save old frame info: push val_int(caller_fp), push val_int(ret_addr)
   where ret_addr = p->pc (instruction after CALL)
   and caller_fp = p->fp
3. Set p->fp = sp - N - 1 - 2 = sp - N - 3
   Wait, that puts fp pointing below the saved info.
   
   Actually let me reconsider. After pushing saved info:
   Stack: ... [closure] [arg0] ... [argN-1] [saved_old_fp] [saved_ret_addr] ← sp
   
   We want fp to point at arg0: fp = position of arg0
   arg0 is at original sp - N, which is now at sp - N - 2 (after pushing 2 more)
   So fp = sp - N - 2
   
   But wait, that means fp+0 = arg0 ✓, fp+1 = arg1 ✓, ..., fp+N-1 = argN-1 ✓
   fp+N = saved_old_fp (no, fp+N = sp-2 which is saved_old_fp)
   fp+N+1 = saved_ret_addr
   
   Closure is at fp-1.
   
   New sp = fp + N (just past the args, ready for locals)
   But we pushed 2 values... so sp is at fp + N + 2. We need to set sp = fp + N.

   Hmm, this is getting confused. Let me be very precise:

   BEFORE: sp points PAST last arg
   stack positions: [sp-N-1]=closure, [sp-N]=arg0, ..., [sp-1]=argN-1
   
   STEP 1: Save old_fp and ret_addr by writing them into the stack
   We can write them at sp and sp+1 (above the args):
   stack[sp] = val_int(p->fp)     // save old_fp
   stack[sp+1] = val_int(p->pc)   // save ret_addr (pc is already past CALL instruction)
   
   STEP 2: Set fp to point at arg0
   p->fp = sp - N    (was sp - N, pointing at arg0... but we also have closure below)
   
   Actually, the args are at indices [sp-N] through [sp-1].
   Closure is at [sp-N-1].
   We write saved info at [sp] and [sp+1].
   
   We want LOAD 0 = arg0 = stack[fp+0], so fp = sp - N.
   Then: fp-1 = closure ✓
         fp+0 = arg0 ✓
         fp+N-1 = argN-1 ✓
         fp+N = old_fp ✓
         fp+N+1 = ret_addr ✓
   
   STEP 3: Set sp for the callee
   p->sp = fp + N + 2  // past old_fp and ret_addr
   (No wait, sp should start where locals will be allocated. The compiler uses next_slot starting at nparams. So sp should point to fp + nparams = fp + N)
   But fp+N is where old_fp is stored! That would overwrite it with locals!
   
   Hmm, I need to rethink. The saved info should go BELOW the args, not above.

### REVISED CALL Convention:

BEFORE CALL: [closure] [arg0] [arg1] ... [argN-1] ← sp

STEP 1: Read closure and args
  closure = stack[sp - N - 1]
  // args stay in place at stack[sp-N] through stack[sp-1]

STEP 2: Shift args down by 2 positions to make room for saved info
  // Move [arg0..argN-1] from [sp-N..sp-1] to [sp-N-2..sp-3]
  memmove(&stack[sp-N-2], &stack[sp-N], N * sizeof(Val));
  // Now: [closure] [empty] [empty] [arg0_moved] ... [argN-1_moved] [arg0_orig] ... [argN-1_orig]

Wait, this is overwriting closure... Let me think again.

Actually, the simplest approach is to NOT use memmove. Instead:

BEFORE CALL: [closure] [arg0] [arg1] ... [argN-1] ← sp

STEP 1: closure = stack[sp - N - 1]  // save closure
STEP 2: stack[sp - N - 1] = val_int(p->fp)   // overwrite closure position with old_fp
STEP 3: stack[sp] = val_int(p->pc)   // push ret_addr above args
        sp++
STEP 4: p->fp = sp - N - 2  // fp points at old_fp... no

Hmm, this is really tricky because the closure, args, and saved info all need to coexist.

Let me use the SIMPLEST possible convention:

### Simplest CALL Convention

BEFORE: [closure] [arg0] ... [argN-1]  ← sp

STEP 1: closure = stack[sp - N - 1]
STEP 2: // Move everything: shift the whole block (closure + args) up by 2
        // to make room for old_fp and ret_addr at the bottom
        // New positions: [old_fp] [ret_addr] [closure] [arg0] ... [argN-1]
        stack[sp + 1] = stack[sp - 1]  // argN-1 (shift up by 2)
        stack[sp] = stack[sp - 2]      // argN-2
        ... general: memmove stack[sp-N+1..sp] = stack[sp-N-1..sp-2]
        
This memmove is messy. Let me use a different approach entirely:

### ALTERNATIVE: Keep closure separately, don't store in frame

BEFORE: [closure] [arg0] ... [argN-1]  ← sp

STEP 1: closure = stack[--sp]  // pop closure (sp now at last arg... no, sp was past last arg)
        Actually: closure = stack[sp - N - 1]; // save it
        sp -= (N + 1); // pop all (closure + args)
        
STEP 2: stack[sp++] = val_int(old_fp)  // push old_fp
        stack[sp++] = val_int(ret_addr) // push ret_addr  
        stack[sp++] = val_int(closure)  // push closure (as int for now, decode later)
        for (i = 0; i < N; i++)
            stack[sp++] = args[i];      // push args back
        
STEP 3: fp = sp - N - 1  // points to closure... 
        Hmm, but compiler does LOAD 0 = arg0, not closure.

OK I think I need to just define:

fp points to the FIRST argument.
Below fp: [old_fp] [ret_addr] [closure]
At/above fp: [arg0] [arg1] ... [argN-1] [locals...]

So:
- fp[-3] = old_fp
- fp[-2] = ret_addr  
- fp[-1] = closure
- fp[0] = arg0
- fp[1] = arg1
- ...
- fp[N-1] = argN-1
- fp[N] = first local (sp starts here)

BEFORE CALL: [closure] [arg0] ... [argN-1]  ← sp

CALL N:
1. closure = stack[sp - N - 1]
2. Shift args down: move stack[sp-N..sp-1] to stack[sp-N-2..sp-3]
   (shift N items down by 2 positions)
   memmove(&stack[sp - N - 2], &stack[sp - N], N * sizeof(Val));
3. Write saved info in the freed slots:
   stack[sp - N - 2] = val_int(p->fp)  → wait, sp-N-2 is where arg0 was moved TO
   
   Let me redo: after the memmove:
   stack[sp-N-2] = arg0 (moved)
   stack[sp-N-1] = arg1 (moved)
   ...
   stack[sp-3] = argN-1 (moved)
   stack[sp-2] = old arg0 (now free)
   stack[sp-1] = old arg1 (now free, only if N>=2)
   
   But we only freed 2 slots (positions sp-N-1 and sp-N, which were the original arg0 and arg1 positions... no, they were closure and arg0 positions)

   Wait: BEFORE: stack[sp-N-1]=closure, stack[sp-N]=arg0, ..., stack[sp-1]=argN-1
   AFTER memmove down by 2: stack[sp-N-3]=arg0, stack[sp-N-2]=arg1, ..., stack[sp-3]=argN-1
   Freed positions: stack[sp-N-1] and stack[sp-2]... no that's wrong.
   
   The memmove moves from [sp-N..sp-1] to [sp-N-2..sp-3]:
   dest_start = sp-N-2, src_start = sp-N, count = N
   After: stack[sp-N-2..sp-3] contains args, stack[sp-2..sp-1] is freed
   And stack[sp-N-1] is where closure was (also freed since we already saved it)
   
   So freed positions are: stack[sp-N-1] and stack[sp-2], stack[sp-1]
   That's 3 freed positions but we only need 2 (old_fp and ret_addr).
   
   Actually stack[sp-N-1] was closure. After the memmove, the 2 positions freed at the top (sp-2, sp-1) plus the closure position gives us 3 slots, but we only need 2.
   
   Hmm, I'm making this way too complicated. Let me just use a simple approach:

### FINAL SIMPLE CALL Convention

Use separate arrays for the stack frame metadata. Or even simpler: just push things in the right order.

BEFORE: [closure] [arg0] ... [argN-1] ← sp

CALL N:
1. Save: old_fp = p->fp, ret_pc = p->pc
2. Pop all: closure = stack[--sp]; // No, closure was pushed first, it's deepest
   
   Actually, let me pop from the top:
   args = malloc(N * sizeof(Val));
   for (int i = N-1; i >= 0; i--) args[i] = stack[--sp];
   closure = stack[--sp];  // pop closure last
   
3. Push frame: push(old_fp), push(ret_pc), push(closure)
4. fp = sp  // fp points to just past closure
5. Push args: for (i=0; i<N; i++) push(args[i])
6. Now: fp[0] = closure (from step 3)
        fp[1] = arg0, fp[2] = arg1, etc.

   But compiler does LOAD 0 = arg0, not LOAD 0 = closure!
   
   So: fp must point to arg0, not closure.
   
   After step 4: fp = sp (points past closure)
   After step 5: arg0 at stack[fp], arg1 at stack[fp+1], etc.
   sp = fp + N
   
   Where are old_fp, ret_pc, closure?
   They are at stack[fp-3], stack[fp-2], stack[fp-1] respectively.
   
   Verification:
   Step 3 pushes 3 values, so sp increased by 3
   Step 4: fp = sp (at this point)
   Step 5 pushes N args, sp = fp + N
   
   old_fp = stack[fp-3] ✓
   ret_pc = stack[fp-2] ✓
   closure = stack[fp-1] ✓
   arg0 = stack[fp+0] ✓
   arg1 = stack[fp+1] ✓
   
   LOAD 0 → stack[fp+0] = arg0 ✓✓✓
   STORE N → stack[fp+N] = first local ✓✓✓

YES! This works! And it's simple!

RET:
1. ret_val = stack[--sp]  // pop return value
2. old_fp = val_get_int(stack[p->fp - 3])
3. ret_pc = val_get_int(stack[p->fp - 2])
4. sp = p->fp - 3  // pop entire frame
5. p->fp = (int)old_fp
6. p->pc = (int)ret_pc
7. stack[sp++] = ret_val  // push return value

TAIL_CALL N:
1. Save new args and closure from stack
   closure = stack[sp - N - 1]
   for (i=0; i<N; i++) new_args[i] = stack[sp - N + i]
   
2. Get caller's saved info:
   caller_fp = val_get_int(stack[p->fp - 3])
   caller_ret = val_get_int(stack[p->fp - 2])
   
3. Reset sp to frame base:
   sp = p->fp - 3
   
4. Restore caller's fp:
   p->fp = caller_fp
   
5. Push new frame (same as CALL steps 3-6):
   push(val_int(caller_fp))  → wait, we're reusing the caller's caller's fp
   push(val_int(caller_ret)) → and returning to caller's return address
   
   Actually for tail call, we want the new function to return to the SAME place as the current function would have returned to. So:
   push(val_int(caller_fp))  → NO, this should be p->fp (the current function's caller_fp)
   
   Wait: after step 4, p->fp is already caller_fp. So:
   push(val_int(p->fp))  → push the caller's fp (which is now our fp)
   push(val_int(caller_ret))  → push the caller's return address
   push(closure)
   p->fp = sp  // set new fp
   push(new_args)
   sp = p->fp + N

Hmm wait, after step 4, p->fp = caller_fp. Then:
push(val_int(p->fp))  → this pushes caller_fp as old_fp for the new frame
push(val_int(caller_ret))  → this pushes caller_ret as ret_addr for the new frame
push(closure)
new_fp = sp
for args: push(new_args[i])
p->fp = new_fp
sp = new_fp + N

So the new frame's old_fp = caller_fp (the original caller's fp)
And the new frame's ret_addr = caller_ret (the original return address)

This is correct! The new function returns directly to the original caller.

## 4. OP_CLOSURE Format (from compile.c)
OP_CLOSURE [fn_id:4 bytes] [nfree:4 bytes] [slot0:4 bytes] [slot1:4 bytes] ...

The closure is allocated on the process heap with:
- entry = fn_table[fn_id] (bytecode offset)
- nfree = number of captured variables
- free[i] = stack[fp + slot_i] (load the captured value from current frame)

## 5. OP_SPAWN Format
OP_SPAWN [fn_id:4 bytes]
Creates new process running function fn_id. Push new pid.

OP_SPAWN_CLOS (no args)
Pop closure from stack. Create new process from that closure. Push new pid.

## 6. Inline Opcodes (from compile.c inline_ops table)
These compile directly, no CALL frame:
+  → OP_ADD (pop 2, push result)
-  → OP_SUB
*  → OP_MUL
/  → OP_DIV
%  → OP_MOD
=  → OP_EQ (pop 2, push val_true/false)
<  → OP_LT
<= → OP_LE
>  → swap operands + OP_LT
>= → swap operands + OP_LE
cons → OP_CONS (pop 2, allocate pair, push)
car → OP_CAR (pop pair, push car)
cdr → OP_CDR (pop pair, push cdr)
null? → OP_IS_NIL (pop, push bool)
pair? → OP_IS_PAIR
int? → OP_IS_INT
string? → OP_IS_STRING
bytes? → OP_IS_BYTES
pid? → OP_IS_PID
print → OP_PRINT (pop, print to stdout)

## 7. Pattern Matching Opcodes
OP_MATCH_INT [value:8 bytes] — compare stack top (via LOAD subj_slot) with value, jump to fail_label if not match
OP_MATCH_SYM [index:4 bytes] — compare stack top with symbol
OP_MATCH_NIL — check if stack top is nil
OP_MATCH_PAIR — check if stack top is pair. If yes, push car and cdr as two new values for sub-pattern matching.

Wait, how does OP_MATCH_PAIR work exactly? Let me check compile.c's pattern compilation...

From compile.c cx_pattern:
For (cons a b) patterns:
1. LOAD subj_slot
2. IS_PAIR, JUMP_IF_FALSE fail_label
3. LOAD subj_slot
4. CAR, STORE car_slot (bind car sub-pattern)
5. LOAD subj_slot  
6. CDR, STORE cdr_slot (bind cdr sub-pattern)
7. cx_pattern(car_pat, car_slot, ...)
8. cx_pattern(cdr_pat, cdr_slot, ...)

Wait, let me re-check... Actually, I see from compile.c:
```
emit_byte(&c->code, OP_LOAD);
emit_int32(&c->code, subj_slot);
emit_byte(&c->code, OP_MATCH_PAIR);
```

So MATCH_PAIR is an instruction that takes the value from LOAD, checks if it's a pair, and if so, provides car and cdr. But how?

Actually, looking at the compile.c code more carefully for how patterns work... let me check the actual pattern compilation code.

From the compile.c, looking at cx_pattern, it seems to use a mix of:
- Direct comparisons (IS_PAIR + JUMP_IF_FALSE)
- MATCH_* opcodes

Let me not guess - instead, let me just have the VM generator read compile.c's pattern section and implement accordingly. The key is the CALL convention which I've now documented precisely.

## 8. Scheduler Loop
```
while runq not empty:
    pid = dequeue()
    proc = procs[pid]
    if proc is NULL or state != RUNNING: continue
    for r in 0..MAX_REDUCTIONS-1:
        if vm_step(vm, proc) < 0: break
    if proc->state == RUNNING: enqueue(pid)
```
MAX_REDUCTIONS = 1000

## 9. Process Death
When process crashes or reaches OP_HALT:
1. Set state = PROC_DEAD
2. For each watcher pid in watchers[]:
   a. Construct DOWN message: (cons 'DOWN (cons ref (cons pid (cons reason nil))))
      Actually: ('DOWN ref pid reason) → a proper list of 4 elements
   b. Deep copy to watcher's heap
   c. Push to watcher's mailbox
   d. If watcher is WAIT_RECV: set to RUNNING, add to runq

## 10. Deep Copy (in val.c)
val_deep_copy(Proc *target, Val v) — recursively copy value to target process heap
Already implemented in val.c. Use it for send.

## 11. Symbol Table (VM)
symbols[] is an array of char* strings. symbol values encode the index.
To add a symbol: check if exists, if not add to table.
To get symbol name: symbols[val_get_symbol(v)]