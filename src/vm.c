/*
 * vm.c — bytecode VM: scheduler, process lifecycle, opcode dispatch
 */

#include "ta.h"
#include <dlfcn.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Thread-local current process — set by worker_loop before executing a proc */
__thread Proc *tls_current_proc = NULL;

/* Match-failure flag. Thread-local: a match sequence runs uninterrupted
 * within one proc's reduction slice on a single worker, so each worker
 * needs its own flag (cannot be shared across workers). */
static __thread int match_ok = 1;
/* ================================================================
 * Yield API — clean interface for C functions to suspend the
 * current proc.  Replaces the old 'would-block magic symbol.
 * ================================================================ */
void vm_watch_fd(VM *vm, int fd, short events) {
    (void)vm;
    Proc *p = tls_current_proc;
    p->wait_fd = fd;
    p->wait_events = events;
}

void vm_yield(VM *vm) { vm->yield_requested = 1; }
/* ================================================================
 * print_val
 * ================================================================ */
void print_val(VM *vm, Val v) {
    if (val_is_int(v)) {
        printf("%lld", (long long)val_get_int(v));
    } else if (val_is_nil(v)) {
        printf("nil");
    } else if (v == val_true()) {
        printf("true");
    } else if (v == val_false()) {
        printf("false");
    } else if (val_is_symbol(v)) {
        printf("%s", vm->symbols[val_get_symbol(v)]);
    } else if (val_is_string(v)) {
        HeapString *s = val_get_string(v);
        printf("%.*s", s->len, s->data);
    } else if (val_is_pair(v)) {
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
    } else if (val_is_pid(v)) {
        printf("<pid %d>", (int)val_get_pid(v));
    } else {
        printf("?");
    }
}

int vm_step(VM *vm, Proc *p) {
    uint8_t op = p->code[p->pc++];

    switch (op) {

    /* ---- stack constants ---- */
    case OP_PUSH_NIL:
        proc_push(p, val_nil());
        break;
    case OP_PUSH_TRUE:
        proc_push(p, val_true());
        break;
    case OP_PUSH_FALSE:
        proc_push(p, val_false());
        break;

    case OP_PUSH_INT8: {
        int8_t i8 = (int8_t)p->code[p->pc++];
        proc_push(p, val_int(i8));
        break;
    }
    case OP_PUSH_INT: {
        int64_t i64;
        memcpy(&i64, &p->code[p->pc], 8);
        p->pc += 8;
        proc_push(p, val_int(i64));
        break;
    }
    case OP_PUSH_SYM: {
        int32_t idx;
        memcpy(&idx, &p->code[p->pc], 4);
        p->pc += 4;
        proc_push(p, val_symbol((uint32_t)idx));
        break;
    }

    /* ---- local variables ---- */
    case OP_LOAD: {
        int32_t off;
        memcpy(&off, &p->code[p->pc], 4);
        p->pc += 4;
        proc_push(p, proc_stack(p)[p->fp + off]);
        break;
    }
    case OP_STORE: {
        int32_t off;
        memcpy(&off, &p->code[p->pc], 4);
        p->pc += 4;
        proc_stack(p)[p->fp + off] = proc_pop(p);
        break;
    }

    /* ---- pair ---- */
    case OP_CONS: {
        Val cdr = proc_pop(p);
        Val car = proc_pop(p);
        proc_push(p, val_pair(p, car, cdr));
        break;
    }
    case OP_CAR: {
        Val v = proc_pop(p);
        if (val_is_nil(v))
            proc_push(p, val_nil());
        else
            proc_push(p, val_get_car(v));
        break;
    }
    case OP_CDR: {
        Val v = proc_pop(p);
        if (val_is_nil(v))
            proc_push(p, val_nil());
        else
            proc_push(p, val_get_cdr(v));
        break;
    }

    /* ---- arithmetic ---- */
    case OP_ADD: {
        Val b = proc_pop(p);
        Val a = proc_pop(p);
        proc_push(p, val_int(val_get_int(a) + val_get_int(b)));
        break;
    }
    case OP_SUB: {
        Val b = proc_pop(p);
        Val a = proc_pop(p);
        proc_push(p, val_int(val_get_int(a) - val_get_int(b)));
        break;
    }
    case OP_MUL: {
        Val b = proc_pop(p);
        Val a = proc_pop(p);
        proc_push(p, val_int(val_get_int(a) * val_get_int(b)));
        break;
    }
    case OP_DIV: {
        Val b = proc_pop(p);
        Val a = proc_pop(p);
        proc_push(p, val_int(val_get_int(a) / val_get_int(b)));
        break;
    }
    case OP_MOD: {
        Val b = proc_pop(p);
        Val a = proc_pop(p);
        proc_push(p, val_int(val_get_int(a) % val_get_int(b)));
        break;
    }

        /* ---- comparison ---- */
    case OP_EQ: {
        Val b = proc_pop(p);
        Val a = proc_pop(p);
        Val result = (a == b) ? val_true() : val_false();
        proc_push(p, result);
        break;
    }
    case OP_LT: {
        Val b = proc_pop(p);
        Val a = proc_pop(p);
        int cmp = val_is_int(a) && val_is_int(b) ? (val_get_int(a) < val_get_int(b)) : 0;
        proc_push(p, cmp ? val_true() : val_false());
        break;
    }
    case OP_LE: {
        Val b = proc_pop(p);
        Val a = proc_pop(p);
        int cmp = val_is_int(a) && val_is_int(b) ? (val_get_int(a) <= val_get_int(b)) : 0;
        proc_push(p, cmp ? val_true() : val_false());
        break;
    }

    /* ---- type tests ---- */
    case OP_IS_NIL: {
        Val v = proc_pop(p);
        proc_push(p, val_is_nil(v) ? val_true() : val_false());
        break;
    }
    case OP_IS_PAIR: {
        Val v = proc_pop(p);
        proc_push(p, val_is_pair(v) ? val_true() : val_false());
        break;
    }
    case OP_IS_INT: {
        Val v = proc_pop(p);
        proc_push(p, val_is_int(v) ? val_true() : val_false());
        break;
    }
    case OP_IS_STRING: {
        Val v = proc_pop(p);
        proc_push(p, val_is_string(v) ? val_true() : val_false());
        break;
    }
    case OP_IS_BYTES: {
        Val v = proc_pop(p);
        proc_push(p, val_is_bytes(v) ? val_true() : val_false());
        break;
    }
    case OP_IS_PID: {
        Val v = proc_pop(p);
        proc_push(p, val_is_pid(v) ? val_true() : val_false());
        break;
    }

    /* ---- control flow ---- */
    case OP_JUMP: {
        int32_t addr;
        memcpy(&addr, &p->code[p->pc], 4);
        p->pc = addr;
        break;
    }
    case OP_JUMP_IF_FALSE: {
        int32_t addr;
        memcpy(&addr, &p->code[p->pc], 4);
        p->pc += 4;
        Val v = proc_pop(p);
        if (val_is_nil(v) || v == val_false())
            p->pc = addr;
        break;
    }
    case OP_POP:
        proc_pop(p);
        break;
    case OP_DUP:
        proc_push(p, proc_peek(p, 0));
        break;

    case OP_PUSH_STRING: {
        int32_t len;
        memcpy(&len, &p->code[p->pc], 4);
        p->pc += 4;
        HeapString *s = (HeapString *)proc_heap_alloc(p, sizeof(HeapString) + len + 1);
        if (!s) {
            proc_push(p, val_nil());
            break;
        }
        s->hdr.type = HEAP_STRING;
        s->hdr.flags = 0;
        s->len = len;
        memcpy(s->data, &p->code[p->pc], len);
        s->data[len] = '\0';
        p->pc += len;
        Val v = ((Val)TAG_STRING << 48) | (uint64_t)(uintptr_t)s;
        proc_push(p, v);
        break;
    }

        /* ---- functions ---- */
    case OP_CLOSURE: {
        int32_t fn_id, nfree;
        memcpy(&fn_id, &p->code[p->pc], 4);
        p->pc += 4;
        memcpy(&nfree, &p->code[p->pc], 4);
        p->pc += 4;
        if (nfree == 0) {
            /* No free vars — encode fn_id directly, no heap alloc */
            Val v = ((Val)TAG_CLOS_ID << 48) | (uint64_t)(uint32_t)fn_id;
            proc_push(p, v);
            break;
        }
        HeapClosure *clos =
            (HeapClosure *)proc_heap_alloc(p, sizeof(HeapClosure) + nfree * (int)sizeof(Val));
        clos->hdr.type = HEAP_CLOS;
        clos->entry = fn_id;
        clos->nfree = nfree;
        for (int i = 0; i < nfree; i++) {
            int32_t off;
            memcpy(&off, &p->code[p->pc], 4);
            p->pc += 4;
            clos->free[i] = proc_stack(p)[p->fp + off];
        }
        Val v = ((Val)TAG_CLOS << 48) | (uint64_t)(uintptr_t)clos;
        proc_push(p, v);
        break;
    }

    case OP_CALL: {
        int32_t nargs;
        memcpy(&nargs, &p->code[p->pc], 4);
        p->pc += 4;
        /* save closure and args from stack */
        Val closure_val = proc_peek(p, nargs);
        if ((closure_val >> 48) != TAG_CLOS && (closure_val >> 48) != TAG_CLOS_ID) {
            /* Find which function contains this pc */
            int fn_id = -1;
            for (int i = 0; i < p->fn_count; i++) {
                int next_start = (i + 1 < p->fn_count) ? p->fn_table[i + 1] : INT_MAX;
                if (p->pc - 4 >= p->fn_table[i] && p->pc - 4 < next_start) {
                    fn_id = i;
                    break;
                }
            }
            fprintf(stderr,
                    "error: cannot call non-function value (tag=0x%04llx, raw=0x%llx, pc=%d, "
                    "fn=%d, nargs=%d)\n",
                    (unsigned long long)(closure_val >> 48), (unsigned long long)closure_val,
                    p->pc - 4, fn_id, nargs);
            proc_die(vm, p, val_nil());
            return -1;
        }
        Val args[256];
        for (int i = 0; i < nargs; i++)
            args[i] = proc_peek(p, nargs - 1 - i);
        /* pop all N+1 items */
        p->sp += nargs + 1;
        int caller_sp = p->sp;
        int ret_pc = p->pc;
        int old_fp = p->fp;

        /* Extract free vars from closure */
        int nfree = 0;
        if ((closure_val >> 48) == TAG_CLOS) {
            HeapClosure *clos = val_as_clos(closure_val);
            nfree = clos->nfree;
        }

        /* Protect C-local Vals from GC/realloc during push loop.
         * After popping args from the TA stack, they exist only in
         * C locals — invisible to gc_collect and gc_fixup_heap_pointers.
         * gc_root_push copies into gc_roots which ARE scanned/fixed. */
        GC_ROOTS_SCOPE(p, rbase) {
            gc_root_push(p, closure_val);
            for (int i = 0; i < nargs; i++)
                gc_root_push(p, args[i]);
            if ((closure_val >> 48) == TAG_CLOS) {
                HeapClosure *clos = val_as_clos(closure_val);
                for (int i = 0; i < nfree; i++)
                    gc_root_push(p, clos->free[i]);
            }

            /* push free vars (at fp+nargs..fp+nargs+nfree-1) */
            for (int i = nfree - 1; i >= 0; i--)
                proc_push(p, p->gc_roots[rbase + 1 + nargs + i]);
            /* push args in reverse order (arg0 at fp+0) */
            for (int i = nargs - 1; i >= 0; i--)
                proc_push(p, p->gc_roots[rbase + 1 + i]);
            /* push header (closure … caller_sp) */
            proc_push(p, p->gc_roots[rbase]); /* closure fp-1 */
            proc_push(p, val_int(ret_pc));    /* fp-2 */
            proc_push(p, val_int(old_fp));    /* fp-3 */
            proc_push(p, val_int(caller_sp)); /* fp-4 */

            /* Restore closure_val (may have been forwarded by GC) */
            closure_val = p->gc_roots[rbase];
        }

        p->fp = caller_sp - nfree - nargs;
        if ((closure_val >> 48) == TAG_CLOS_ID)
            p->pc = p->fn_table[(int)(closure_val & 0xFFFFFFFFFFFFULL)];
        else {
            HeapClosure *clos = val_as_clos(closure_val);
            p->pc = p->fn_table[clos->entry];
        }
        break;
    }

    case OP_TAIL_CALL: {
        int32_t nargs;
        memcpy(&nargs, &p->code[p->pc], 4);
        p->pc += 4;
        Val closure_val = proc_peek(p, nargs);
        if ((closure_val >> 48) != TAG_CLOS && (closure_val >> 48) != TAG_CLOS_ID) {
            fprintf(stderr, "error: cannot call non-function value\n");
            proc_die(vm, p, val_nil());
            return -1;
        }
        Val args[256];
        for (int i = 0; i < nargs; i++)
            args[i] = proc_peek(p, nargs - 1 - i);
        /* current frame's caller info */
        int caller_sp = (int)val_get_int(proc_stack(p)[p->fp - 4]);
        int old_fp = (int)val_get_int(proc_stack(p)[p->fp - 3]);
        int ret_pc = (int)val_get_int(proc_stack(p)[p->fp - 2]);
        /* pop new closure + args */
        p->sp += nargs + 1;
        /* restore caller's frame */
        p->sp = caller_sp;
        p->fp = old_fp;

        /* Extract free vars from closure */
        int nfree = 0;
        if ((closure_val >> 48) == TAG_CLOS) {
            HeapClosure *clos = val_as_clos(closure_val);
            nfree = clos->nfree;
        }

        /* Protect C-local Vals from GC/realloc during push loop */
        int CS;
        GC_ROOTS_SCOPE(p, rbase) {
            gc_root_push(p, closure_val);
            for (int i = 0; i < nargs; i++)
                gc_root_push(p, args[i]);
            if ((closure_val >> 48) == TAG_CLOS) {
                HeapClosure *clos = val_as_clos(closure_val);
                for (int i = 0; i < nfree; i++)
                    gc_root_push(p, clos->free[i]);
            }

            /* push new call from caller's perspective */
            CS = p->sp;
            for (int i = nfree - 1; i >= 0; i--)
                proc_push(p, p->gc_roots[rbase + 1 + nargs + i]);
            for (int i = nargs - 1; i >= 0; i--)
                proc_push(p, p->gc_roots[rbase + 1 + i]);
            proc_push(p, p->gc_roots[rbase]); /* closure */
            proc_push(p, val_int(ret_pc));
            proc_push(p, val_int(old_fp));
            proc_push(p, val_int(CS));

            /* Restore closure_val (may have been forwarded by GC) */
            closure_val = p->gc_roots[rbase];
        }

        p->fp = CS - nfree - nargs;
        if ((closure_val >> 48) == TAG_CLOS_ID)
            p->pc = p->fn_table[(int)(closure_val & 0xFFFFFFFFFFFFULL)];
        else {
            HeapClosure *clos = val_as_clos(closure_val);
            p->pc = p->fn_table[clos->entry];
        }
        break;
    }

    case OP_RET: {
        Val ret_val = proc_pop(p);
        int caller_sp = (int)val_get_int(proc_stack(p)[p->fp - 4]);
        int old_fp = (int)val_get_int(proc_stack(p)[p->fp - 3]);
        int ret_addr = (int)val_get_int(proc_stack(p)[p->fp - 2]);

        p->sp = caller_sp;
        p->fp = old_fp;
        if (ret_addr < 0) {
            proc_die(vm, p, val_nil());
            return -1;
        }
        p->pc = ret_addr;
        proc_push(p, ret_val);
        break;
    }

    case OP_ENTER: {
        /* Reserve stack space for local variables.
         * Pushes nslots nil values so that GC sees safe values
         * and the parent's stack is not overwritten. */
        int32_t nslots;
        memcpy(&nslots, &p->code[p->pc], 4);
        p->pc += 4;
        for (int i = 0; i < nslots; i++)
            proc_push(p, val_nil());
        break;
    }

        /* ---- actor primitives ---- */
    case OP_SPAWN:
    case OP_SPAWN_MAIN: {
        int32_t fn_id;
        memcpy(&fn_id, &p->code[p->pc], 4);
        p->pc += 4;

        Proc *np = proc_new(vm);
        proc_ensure_heap(np);
        np->fp = -4;
        np->sp = -8;
        proc_stack(np)[np->fp - 1] = val_nil();
        proc_stack(np)[np->fp - 2] = val_int(-1);
        proc_stack(np)[np->fp - 3] = val_int(0);
        proc_stack(np)[np->fp - 4] = val_int(np->sp);
        np->pc = np->fn_table[fn_id];
        runq_enqueue(vm, np->pid);
        /* Only OP_SPAWN_MAIN (compiler-spawned main()) sets main_pid.
         * Regular spawn from user code never changes main_pid. */
        if (op == OP_SPAWN_MAIN) {
            vm->main_pid = np->pid;
        }
        proc_push(p, val_pid(np->pid));
        break;
    }

    case OP_SPAWN_CLOS: {
        Val clos_val = proc_pop(p);
        Proc *np = proc_new(vm);
        proc_ensure_heap(np);

        /* Extract free vars from closure */
        Val free_vals[256];
        int nfree = 0;
        if ((clos_val >> 48) == TAG_CLOS) {
            HeapClosure *clos = val_as_clos(clos_val);
            nfree = clos->nfree;
            for (int i = 0; i < nfree; i++)
                free_vals[i] = val_deep_copy(np, clos->free[i]);
        }

        /* Set up frame: free vars at fp+0..fp+nfree-1, header at fp-1..fp-4 */
        np->sp = 0;
        for (int i = nfree - 1; i >= 0; i--)
            proc_push(np, free_vals[i]);
        /* push header */
        proc_push(np, clos_val);        /* fp-1 */
        proc_push(np, val_int(-1));     /* fp-2: ret_pc sentinel */
        proc_push(np, val_int(0));      /* fp-3: old_fp */
        proc_push(np, val_int(np->sp)); /* fp-4: caller_sp */
        np->fp = -nfree;                /* fp+0 = first free var */

        if ((clos_val >> 48) == TAG_CLOS_ID)
            np->pc = np->fn_table[(int)(clos_val & 0xFFFFFFFFFFFFULL)];
        else {
            HeapClosure *clos = val_as_clos(clos_val);
            np->pc = np->fn_table[clos->entry];
        }
        runq_enqueue(vm, np->pid);
        proc_push(p, val_pid(np->pid));
        break;
    }

    case OP_SEND: {
        Val pid_v = proc_pop(p); /* pid pushed last → on top */
        Val msg = proc_pop(p);   /* msg pushed first */
        Proc *t = vm->procs[val_get_pid(pid_v)];
        if (t && t->state != PROC_DEAD) {
            /* mbox_deliver serializes msg into a malloc'd fragment on the
             * sender's side and wakes the target under its mbox_lock if
             * blocked on recv (enqueue-at-most-once → Skynet invariant). */
            mbox_deliver(vm, t, msg);
        }
        proc_push(p, val_nil()); /* send returns nil to keep stack balanced */
        break;
    }

    case OP_RECV: {
        if (p->mbox_count == 0) {
            p->pc--; /* rewind so OP_RECV re-executes on resume */
            p->state = PROC_WAIT_RECV;
            return -1;
        }
        proc_push(p, mbox_pop(p));
        break;
    }

    /* Selective receive: peek the next mailbox fragment (without
     * removing it) deep-copied onto this proc's heap. The compiler
     * stores it in a temp slot and runs pattern code against it.
     * - If a fragment exists: push it, advance peek_index, and reset
     *   match_ok so the following pattern sequence starts clean.
     * - If the mailbox is exhausted: rewind to this opcode, block on
     *   recv. peek_index is preserved so a resumed scan (after a new
     *   message arrives) only inspects unseen messages — already-skipped
     *   fragments don't match the (immutable) patterns, so skipping them
     *   forever is correct, and they stay for a future receive. */
    case OP_RECV_PEEK: {
        match_ok = 1;
        pthread_mutex_lock(&p->mbox_lock);
        if (p->peek_index < p->mbox_count) {
            MsgFragment *frag = p->mbox_frag_head;
            for (int i = 0; i < p->peek_index; i++)
                frag = frag->next;
            Val msg = val_deep_copy(p, frag->root);
            p->peek_index++;
            pthread_mutex_unlock(&p->mbox_lock);
            proc_push(p, msg);
        } else {
            pthread_mutex_unlock(&p->mbox_lock);
            p->pc--; /* re-execute OP_RECV_PEEK on wake */
            p->state = PROC_WAIT_RECV;
            return -1;
        }
        break;
    }

    /* A pattern matched: drop the fragment we just peeked (at
     * peek_index-1) from the mailbox and reset the scan cursor. The
     * matched message's heap copy was already consumed by the pattern
     * (bound to variables); the fragment itself is freed here. */
    case OP_RECV_COMMIT: {
        int target = p->peek_index - 1;
        pthread_mutex_lock(&p->mbox_lock);
        if (target == 0) {
            MsgFragment *frag = p->mbox_frag_head;
            p->mbox_frag_head = frag->next;
            if (!p->mbox_frag_head)
                p->mbox_frag_tail = NULL;
            free(frag);
        } else {
            MsgFragment *prev = p->mbox_frag_head;
            for (int i = 0; i < target - 1; i++)
                prev = prev->next;
            MsgFragment *frag = prev->next;
            prev->next = frag->next;
            if (frag == p->mbox_frag_tail)
                p->mbox_frag_tail = prev;
            free(frag);
        }
        p->mbox_count--;
        p->peek_index = 0;
        pthread_mutex_unlock(&p->mbox_lock);
        break;
    }

    case OP_SELF:
        proc_push(p, val_pid((uint32_t)p->pid));
        break;

    case OP_MONITOR: {
        Val pid_v = proc_pop(p);
        uint32_t tpid = val_get_pid(pid_v);
        Proc *t = (tpid < (uint32_t)vm->procs_cap) ? vm->procs[tpid] : NULL;
        int ref = ++vm->next_ref;
        if (t && t->state != PROC_DEAD) {
            /* Normal path: join watchers, DOWN sent when target dies */
            if (t->watcher_count >= t->watcher_cap) {
                t->watcher_cap = t->watcher_cap ? t->watcher_cap * 2 : 4;
                t->watchers = realloc(t->watchers, t->watcher_cap * sizeof(int));
                t->watcher_refs = realloc(t->watcher_refs, t->watcher_cap * sizeof(Val));
            }
            t->watchers[t->watcher_count] = p->pid;
            t->watcher_refs[t->watcher_count] = val_int(ref);
            t->watcher_count++;
            /* Double-check: target may have died between our state check and
             * the watcher insertion above.  If proc_die ran concurrently it
             * would have seen watcher_count BEFORE the increment and skipped
             * sending DOWN — so we must deliver it here. */
            if (t->state == PROC_DEAD) {
                int down_sym = vm_intern_symbol(vm, "DOWN");
                int noproc_sym = vm_intern_symbol(vm, "noproc");
                Val msg = val_pair(
                    p, val_symbol((uint32_t)down_sym),
                    val_pair(p, val_int(ref),
                             val_pair(p, val_pid(tpid),
                                      val_pair(p, val_symbol((uint32_t)noproc_sym), val_nil()))));
                mbox_deliver(vm, p, msg);
            }
        } else {
            /* Target already dead or nonexistent: deliver DOWN immediately */
            int down_sym = vm_intern_symbol(vm, "DOWN");
            int noproc_sym = vm_intern_symbol(vm, "noproc");
            Val msg = val_pair(
                p, val_symbol((uint32_t)down_sym),
                val_pair(p, val_int(ref),
                         val_pair(p, val_pid(tpid),
                                  val_pair(p, val_symbol((uint32_t)noproc_sym), val_nil()))));
            mbox_deliver(vm, p, msg);
        }
        proc_push(p, val_int(ref));
        break;
    }

        /* ---- built-in ---- */
    case OP_PRINT: {
        Val v = proc_pop(p);
        print_val(vm, v);
        printf("\n");
        fflush(stdout);
        proc_push(p, val_nil());
        break;
    }
    case OP_HALT:
        vm->eval_result = proc_peek(p, 0);
        proc_die(vm, p, val_nil());
        return -1;

    /* ---- pattern matching ---- */
    case OP_MATCH_INT: {
        int64_t expected;
        memcpy(&expected, &p->code[p->pc], 8);
        p->pc += 8;
        if (!match_ok)
            break;
        Val v = proc_pop(p);
        if (val_is_int(v) && val_get_int(v) == expected) {
            /* consumed */
        } else {
            proc_push(p, v);
            match_ok = 0;
        }
        break;
    }
    case OP_MATCH_SYM: {
        int32_t idx;
        memcpy(&idx, &p->code[p->pc], 4);
        p->pc += 4;
        if (!match_ok)
            break;
        Val v = proc_pop(p);
        if (val_is_symbol(v) && val_get_symbol(v) == (uint32_t)idx) {
            /* consumed */
        } else {
            proc_push(p, v);
            match_ok = 0;
        }
        break;
    }
    case OP_MATCH_NIL: {
        if (!match_ok)
            break;
        Val v = proc_pop(p);
        if (val_is_nil(v)) {
            /* consumed */
        } else {
            proc_push(p, v);
            match_ok = 0;
        }
        break;
    }
    case OP_MATCH_PAIR: {
        if (!match_ok)
            break;
        Val v = proc_pop(p);
        if (val_is_pair(v)) {
            proc_push(p, val_get_cdr(v));
            proc_push(p, val_get_car(v));
        } else {
            proc_push(p, v);
            match_ok = 0;
        }
        break;
    }
    case OP_MATCH_JUMP: {
        int32_t addr;
        memcpy(&addr, &p->code[p->pc], 4);
        p->pc += 4;
        if (!match_ok) {
            p->pc = addr;
            match_ok = 1;
        }
        break;
    }

    /* ---- string builtins ---- */
    case OP_STR_LEN: {
        Val s = proc_pop(p);
        if (val_tag(s) != TAG_STRING) {
            proc_push(p, val_nil());
            break;
        }
        HeapString *hs = val_get_string(s);
        proc_push(p, val_int(hs->len));
        break;
    }
    case OP_STR_CONCAT: {
        Val s2 = proc_pop(p);
        Val s1 = proc_pop(p);
        if (val_tag(s1) != TAG_STRING || val_tag(s2) != TAG_STRING) {
            proc_push(p, val_nil());
            break;
        }
        HeapString *h1 = val_get_string(s1);
        HeapString *h2 = val_get_string(s2);
        /* Extract data to C locals BEFORE any allocation (GC safety) */
        int len1 = h1->len, len2 = h2->len;
        int total_len = len1 + len2;
        char *tmp = malloc(total_len + 1);
        if (!tmp) {
            proc_push(p, val_nil());
            break;
        }
        memcpy(tmp, h1->data, len1);
        memcpy(tmp + len1, h2->data, len2);
        tmp[total_len] = '\0';
        Val result = val_string(p, tmp, total_len);
        free(tmp);
        proc_push(p, result);
        break;
    }
    case OP_STR_SLICE: {
        Val vend = proc_pop(p);
        Val vstart = proc_pop(p);
        Val s = proc_pop(p);
        if (val_tag(s) != TAG_STRING) {
            proc_push(p, val_nil());
            break;
        }
        HeapString *hs = val_get_string(s);
        int start = (int)val_get_int(vstart);
        int end = (int)val_get_int(vend);
        if (start < 0)
            start = 0;
        if (end > hs->len)
            end = hs->len;
        if (start >= end) {
            proc_push(p, val_string(p, "", 0));
            break;
        }
        /* Extract before allocating */
        int slen = end - start;
        char tmp[slen + 1];
        memcpy(tmp, hs->data + start, slen);
        tmp[slen] = '\0';
        Val result = val_string(p, tmp, slen);
        proc_push(p, result);
        break;
    }
    case OP_STR_EQ: {
        Val s2 = proc_pop(p);
        Val s1 = proc_pop(p);
        if (val_tag(s1) != TAG_STRING || val_tag(s2) != TAG_STRING) {
            proc_push(p, val_nil());
            break;
        }
        HeapString *h1 = val_get_string(s1);
        HeapString *h2 = val_get_string(s2);
        int eq = (h1->len == h2->len && memcmp(h1->data, h2->data, h1->len) == 0);
        proc_push(p, eq ? val_true() : val_nil());
        break;
    }
    /* 54 was OP_CCALL (index-based) — removed, use OP_CCALL_NAME */
    case OP_CCALL_NAME: {
        int pc_start = p->pc - 1; /* save for rewind on yield */
        int sym_idx;
        memcpy(&sym_idx, p->code + p->pc, 4);
        p->pc += 4;
        uint8_t nc = p->code[p->pc++];
        if (sym_idx < 0 || sym_idx >= vm->sym_count) {
            for (int i = 0; i < nc; i++)
                proc_pop(p);
            proc_push(p, val_nil());
            break;
        }
        if (nc > 64) {
            for (int i = 0; i < nc; i++)
                proc_pop(p);
            proc_push(p, val_nil());
            break;
        }
        const char *name = vm->symbols[sym_idx];
        int cfidx = vm_find_cfunc(vm, name);
        if (cfidx < 0) {
            /* Erlang-style auto-load: if name contains a dot, try dlopen
             * lib/<module>.so and retry the lookup. */
            const char *dot = strchr(name, '.');
            if (dot) {
                int mod_len = (int)(dot - name);
                char mod_path[256];
                int n = snprintf(mod_path, sizeof(mod_path), "lib/%.*s.so", mod_len, name);
                if (n > 0 && n < (int)sizeof(mod_path)) {
                    void *handle = dlopen(mod_path, RTLD_NOW | RTLD_GLOBAL);
                    if (handle) {
                        void (*reg)(VM *) = (void (*)(VM *))dlsym(handle, "vm_load_self");
                        if (reg)
                            reg(vm);
                        cfidx = vm_find_cfunc(vm, name);
                    }
                }
            }
            if (cfidx < 0) {
                for (int i = 0; i < nc; i++)
                    proc_pop(p);
                proc_push(p, val_nil());
                break;
            }
        }
        Val args[64];
        for (int i = nc - 1; i >= 0; i--)
            args[i] = proc_pop(p);
        tls_current_proc = p;
        vm->yield_requested = 0;
        Val result = vm->cfuncs[cfidx].fn(vm, args, nc);
        if (vm->yield_requested) {
            for (int i = 0; i < nc; i++)
                proc_push(p, args[i]);
            p->state = PROC_WAIT_IO;
            p->pc = pc_start;
            return -1;
        }
        proc_push(p, result);
        break;
    }

    default:
        fprintf(stderr, "vm_step: unknown opcode %d at pc=%d\n", op, p->pc - 1);
        proc_die(vm, p, val_nil());
        return -1;
    }

    return 0;
}