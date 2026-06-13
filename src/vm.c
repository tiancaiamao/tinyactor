/*
 * vm.c — bytecode VM: scheduler, process lifecycle, opcode dispatch
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include "ta.h"

/* ----------------------------------------------------------------
 * Forward declarations (static helpers)
 * ---------------------------------------------------------------- */
static void  mbox_push(Proc *p, Val msg);
static Val   mbox_pop(Proc *p);
static void  runq_enqueue(VM *vm, int pid);
static Proc *proc_new(VM *vm);
static void  proc_die(VM *vm, Proc *p, Val reason);

/* Match-failure flag (single-threaded VM) */
static int match_ok = 1;

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

/* ================================================================
 * Mailbox (FIFO circular buffer)
 * ================================================================ */
static void mbox_push(Proc *p, Val msg) {
    if (p->mbox_count >= p->mbox_cap) {
        p->mbox_cap *= 2;
        p->mbox = realloc(p->mbox, p->mbox_cap * sizeof(Val));
    }
    p->mbox[p->mbox_tail % p->mbox_cap] = msg;
    p->mbox_tail++;
    p->mbox_count++;
}

static Val mbox_pop(Proc *p) {
    Val msg = p->mbox[p->mbox_head % p->mbox_cap];
    p->mbox_head++;
    p->mbox_count--;
    return msg;
}

/* ================================================================
 * Run queue
 * ================================================================ */
static void runq_enqueue(VM *vm, int pid) {
    if (vm->rq_tail - vm->rq_head >= vm->rq_cap) {
        int new_cap = vm->rq_cap * 2;
        int *new_q  = malloc(new_cap * sizeof(int));
        for (int i = vm->rq_head; i < vm->rq_tail; i++)
            new_q[i - vm->rq_head] = vm->runq[i % vm->rq_cap];
        free(vm->runq);
        vm->runq    = new_q;
        vm->rq_cap  = new_cap;
        vm->rq_head = 0;
        vm->rq_tail = vm->rq_tail /* preserve count */;
    }
    vm->runq[vm->rq_tail % vm->rq_cap] = pid;
    vm->rq_tail++;
}

/* ================================================================
 * Process lifecycle
 * ================================================================ */
static Proc *proc_new(VM *vm) {
    Proc *p = calloc(1, sizeof(Proc));
    p->pid   = vm->next_pid++;
    p->state = PROC_RUNNING;

    /* grow procs array if needed */
    if (p->pid >= vm->procs_cap) {
        int new_cap = vm->procs_cap ? vm->procs_cap * 2 : 64;
        while (p->pid >= new_cap) new_cap *= 2;
        vm->procs = realloc(vm->procs, new_cap * sizeof(Proc *));
        memset(vm->procs + vm->procs_cap, 0,
               (new_cap - vm->procs_cap) * sizeof(Proc *));
        vm->procs_cap = new_cap;
    }
    vm->procs[p->pid] = p;
    vm->procs_count++;

        /* execution context */
    p->mem_size = 65536;
    p->mem      = malloc(p->mem_size);
    p->gc_to    = calloc(1, p->mem_size);
    p->heap_ptr = 0;
    p->sp       = 0;
    p->fp       = 0;
    p->pc       = 0;
    p->gc_root_count = 0;

    /* shared bytecode */
    p->code     = vm->code;
    p->fn_table = vm->fn_table;
    p->fn_count = vm->fn_count;

    /* mailbox */
    p->mbox_cap = 16;
    p->mbox     = malloc(16 * sizeof(Val));

    /* watchers */
    p->watcher_cap  = 4;
    p->watchers     = malloc(4 * sizeof(int));
    p->watcher_refs = malloc(4 * sizeof(Val));

    return p;
}

/* proc_free is provided externally or in vm_free implementation */

static void proc_die(VM *vm, Proc *p, Val reason) {
    int was_wait_io = (p->state == PROC_WAIT_IO);
    p->state = PROC_DEAD;
    if (was_wait_io && p->wait_fd >= 0) {
        close(p->wait_fd);
        p->wait_fd = -1;
    }
    for (int i = 0; i < p->watcher_count; i++) {
        int  wid = p->watchers[i];
        Proc *w  = vm->procs[wid];
        if (!w || w->state == PROC_DEAD) continue;
        /* Build ('DOWN ref pid reason) as a linked list */
        int down_sym = vm_intern_symbol(vm, "DOWN");
        Val msg = val_pair(w,
            val_symbol((uint32_t)down_sym),
            val_pair(w,
                p->watcher_refs[i],
                val_pair(w,
                    val_pid(p->pid),
                    val_pair(w,
                        val_deep_copy(w, reason),
                        val_nil()))));
        mbox_push(w, msg);
        if (w->state == PROC_WAIT_RECV) {
            w->state = PROC_RUNNING;
            runq_enqueue(vm, w->pid);
        }
    }
}

/* ================================================================
 * Public: spawn a process running fn_id
 * ================================================================ */
int vm_spawn(VM *vm, int fn_id) {
    Proc *np  = proc_new(vm);
    /* Set up initial frame so fp is negative, allowing local var
       slots (fp+offset) to stay within the stack. */
    np->fp = -4;
    np->sp = -8;
    proc_stack(np)[np->fp - 1] = val_nil();      /* closure  */
    proc_stack(np)[np->fp - 2] = val_int(-1);    /* ret_pc sentinel */
    proc_stack(np)[np->fp - 3] = val_int(0);     /* old_fp   */
    proc_stack(np)[np->fp - 4] = val_int(np->sp);/* caller_sp*/
    np->pc    = np->fn_table[fn_id];
    runq_enqueue(vm, np->pid);
    return np->pid;
}

/* ================================================================
 * Scheduler
 * ================================================================ */
#define MAX_REDUCTIONS 1000

void vm_run(VM *vm) {
    int stall = 0;
    for (;;) {
        /* Phase 1: run all ready processes */
        int ran = 0;
        while (vm->rq_head != vm->rq_tail) {
            int  pid = vm->runq[vm->rq_head % vm->rq_cap];
            vm->rq_head++;
            Proc *p = vm->procs[pid];
            if (!p || p->state != PROC_RUNNING) continue;
            ran = 1;
            ProcState prev_state = p->state;
            vm->current_proc = p;
            for (int r = 0; r < MAX_REDUCTIONS; r++) {
                if (vm_step(vm, p) != 0) break;
            }
            if (p->state == PROC_RUNNING)
                runq_enqueue(vm, p->pid);

            if (p->state != prev_state)
                stall = 0;
            else
                stall++;
            if (stall > 10000) {
                for (int i = 0; i < vm->procs_cap; i++) {
                    Proc *q = vm->procs[i];
                    if (q && q->state == PROC_RUNNING)
                        q->state = PROC_DEAD;
                }
                vm->current_proc = NULL;
                return;
            }
        }

                /* Phase 2: collect WAIT_IO processes and poll */
        struct pollfd pfds[1024];
        int           pids[1024];
        int           nfds = 0;

        for (int i = 0; i < vm->procs_cap && nfds < 1024; i++) {
            Proc *p = vm->procs[i];
            if (p && p->state == PROC_WAIT_IO) {
                pfds[nfds].fd      = p->wait_fd;
                pfds[nfds].events  = p->wait_events;
                pfds[nfds].revents = 0;
                pids[nfds]         = p->pid;
                nfds++;
            }
        }

        if (nfds == 0) {
            /* No ready processes and no I/O waits → done */
            break;
        }

        /* No ready processes ran, but some are waiting on I/O */
        if (!ran) {
            poll(pfds, (nfds_t)nfds, 100);  /* 100ms timeout */

            /* Wake processes whose fds are ready */
            for (int i = 0; i < nfds; i++) {
                if (pfds[i].revents & (POLLIN | POLLOUT | POLLERR | POLLHUP)) {
                    Proc *p = vm->procs[pids[i]];
                    if (p && p->state == PROC_WAIT_IO) {
                        p->state = PROC_RUNNING;
                        runq_enqueue(vm, p->pid);
                    }
                }
            }
        }
    }
    vm->current_proc = NULL;
}

/* ================================================================
 * Opcode dispatch — vm_step
 * Returns 0 on success, -1 to break out of the reduction loop.
 * ================================================================ */
int vm_step(VM *vm, Proc *p) {
    uint8_t op = p->code[p->pc++];

    switch (op) {

    /* ---- stack constants ---- */
    case OP_PUSH_NIL:   proc_push(p, val_nil());   break;
    case OP_PUSH_TRUE:  proc_push(p, val_true());  break;
    case OP_PUSH_FALSE: proc_push(p, val_false()); break;

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
        memcpy(&off, &p->code[p->pc], 4); p->pc += 4;
        proc_push(p, proc_stack(p)[p->fp + off]);
        break;
    }
    case OP_STORE: {
        int32_t off;
        memcpy(&off, &p->code[p->pc], 4); p->pc += 4;
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
        proc_push(p, val_get_car(v));
        break;
    }
    case OP_CDR: {
        Val v = proc_pop(p);
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
        proc_push(p, (a == b) ? val_true() : val_false());
        break;
    }
    case OP_LT: {
        Val b = proc_pop(p);
        Val a = proc_pop(p);
        int cmp = val_is_int(a) && val_is_int(b)
                      ? (val_get_int(a) < val_get_int(b))
                      : 0;
        proc_push(p, cmp ? val_true() : val_false());
        break;
    }
    case OP_LE: {
        Val b = proc_pop(p);
        Val a = proc_pop(p);
        int cmp = val_is_int(a) && val_is_int(b)
                      ? (val_get_int(a) <= val_get_int(b))
                      : 0;
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
        proc_push(p, val_is_pid_type(v) ? val_true() : val_false());
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
        memcpy(&addr, &p->code[p->pc], 4); p->pc += 4;
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
        memcpy(&len, &p->code[p->pc], 4); p->pc += 4;
        HeapString *s = (HeapString *)proc_heap_alloc(p,
            sizeof(HeapString) + len + 1);
        if (!s) { proc_push(p, val_nil()); break; }
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
        memcpy(&fn_id,  &p->code[p->pc], 4); p->pc += 4;
        memcpy(&nfree,  &p->code[p->pc], 4); p->pc += 4;
        if (nfree == 0) {
            /* No free vars — encode fn_id directly, no heap alloc */
            Val v = ((Val)TAG_CLOS_ID << 48) | (uint64_t)(uint32_t)fn_id;
            proc_push(p, v);
            break;
        }
        HeapClosure *clos = (HeapClosure *)proc_heap_alloc(
            p, sizeof(HeapClosure) + nfree * (int)sizeof(Val));
        clos->hdr.type = HEAP_CLOS;
        clos->entry    = fn_id;
        clos->nfree    = nfree;
        for (int i = 0; i < nfree; i++) {
            int32_t off;
            memcpy(&off, &p->code[p->pc], 4); p->pc += 4;
            clos->free[i] = proc_stack(p)[p->fp + off];
        }
        Val v = ((Val)TAG_CLOS << 48) | (uint64_t)(uintptr_t)clos;
        proc_push(p, v);
        break;
    }

        case OP_CALL: {
        int32_t nargs;
        memcpy(&nargs, &p->code[p->pc], 4); p->pc += 4;
        /* save closure and args from stack */
        Val closure_val = proc_peek(p, nargs);
        Val args[256];
        for (int i = 0; i < nargs; i++)
            args[i] = proc_peek(p, nargs - 1 - i);
        /* pop all N+1 items */
        p->sp += nargs + 1;
        int caller_sp = p->sp;
        int ret_pc    = p->pc;
        int old_fp    = p->fp;

        /* Extract free vars from closure */
        Val free_vals[256];
        int nfree = 0;
        if ((closure_val >> 48) == TAG_CLOS) {
            HeapClosure *clos = val_as_clos(closure_val);
            nfree = clos->nfree;
            for (int i = 0; i < nfree; i++)
                free_vals[i] = clos->free[i];
        }

        /* push free vars (at fp+nargs..fp+nargs+nfree-1) */
        for (int i = nfree - 1; i >= 0; i--)
            proc_push(p, free_vals[i]);
        /* push args in reverse order (arg0 at fp+0) */
        for (int i = nargs - 1; i >= 0; i--)
            proc_push(p, args[i]);
        /* push header (closure … caller_sp) */
        proc_push(p, closure_val);        /* fp-1 */
        proc_push(p, val_int(ret_pc));    /* fp-2 */
        proc_push(p, val_int(old_fp));    /* fp-3 */
        proc_push(p, val_int(caller_sp)); /* fp-4 */
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
        memcpy(&nargs, &p->code[p->pc], 4); p->pc += 4;
        Val closure_val = proc_peek(p, nargs);
        Val args[256];
        for (int i = 0; i < nargs; i++)
            args[i] = proc_peek(p, nargs - 1 - i);
        /* current frame's caller info */
        int caller_sp = (int)val_get_int(proc_stack(p)[p->fp - 4]);
        int old_fp    = (int)val_get_int(proc_stack(p)[p->fp - 3]);
        int ret_pc    = (int)val_get_int(proc_stack(p)[p->fp - 2]);
        /* pop new closure + args */
        p->sp += nargs + 1;
        /* restore caller's frame */
        p->sp = caller_sp;
        p->fp = old_fp;

        /* Extract free vars from closure */
        Val free_vals[256];
        int nfree = 0;
        if ((closure_val >> 48) == TAG_CLOS) {
            HeapClosure *clos = val_as_clos(closure_val);
            nfree = clos->nfree;
            for (int i = 0; i < nfree; i++)
                free_vals[i] = clos->free[i];
        }

        /* push new call from caller's perspective */
        int CS = p->sp;
        for (int i = nfree - 1; i >= 0; i--)
            proc_push(p, free_vals[i]);
        for (int i = nargs - 1; i >= 0; i--)
            proc_push(p, args[i]);
        proc_push(p, closure_val);
        proc_push(p, val_int(ret_pc));
        proc_push(p, val_int(old_fp));
        proc_push(p, val_int(CS));
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
        Val ret_val   = proc_pop(p);
        int caller_sp = (int)val_get_int(proc_stack(p)[p->fp - 4]);
                int old_fp    = (int)val_get_int(proc_stack(p)[p->fp - 3]);
        int ret_addr  = (int)val_get_int(proc_stack(p)[p->fp - 2]);
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

    /* ---- actor primitives ---- */
                                                                case OP_SPAWN: {
        int32_t fn_id;
        memcpy(&fn_id, &p->code[p->pc], 4); p->pc += 4;
        Proc *np = proc_new(vm);
        np->fp = -4;
        np->sp = -8;
        proc_stack(np)[np->fp - 1] = val_nil();
        proc_stack(np)[np->fp - 2] = val_int(-1);
        proc_stack(np)[np->fp - 3] = val_int(0);
        proc_stack(np)[np->fp - 4] = val_int(np->sp);
        np->pc = np->fn_table[fn_id];
                runq_enqueue(vm, np->pid);
        proc_push(p, val_pid(np->pid));
        break;
    }

                                                case OP_SPAWN_CLOS: {
        Val clos_val = proc_pop(p);
        Proc *np = proc_new(vm);

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
        proc_push(np, clos_val);            /* fp-1 */
        proc_push(np, val_int(-1));         /* fp-2: ret_pc sentinel */
        proc_push(np, val_int(0));          /* fp-3: old_fp */
        proc_push(np, val_int(np->sp));     /* fp-4: caller_sp */
        np->fp = -nfree;                    /* fp+0 = first free var */

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
        Val pid_v = proc_pop(p);  /* pid pushed last → on top */
        Val msg   = proc_pop(p);  /* msg pushed first */
        Proc *t   = vm->procs[val_get_pid(pid_v)];
        if (t && t->state != PROC_DEAD) {
            mbox_push(t, val_deep_copy(t, msg));
            if (t->state == PROC_WAIT_RECV) {
                t->state = PROC_RUNNING;
                runq_enqueue(vm, t->pid);
            }
        }
        break;
    }

                        case OP_RECV: {
        if (p->mbox_count == 0) {
            p->pc--;  /* rewind so OP_RECV re-executes on resume */
            p->state = PROC_WAIT_RECV;
            return -1;
        }
        proc_push(p, mbox_pop(p));
        break;
    }

    case OP_SELF:
        proc_push(p, val_pid((uint32_t)p->pid));
        break;

            case OP_MONITOR: {
        Val pid_v = proc_pop(p);
        uint32_t tpid = val_get_pid(pid_v);
        Proc *t = (tpid < (uint32_t)vm->procs_cap) ? vm->procs[tpid] : NULL;
        if (t) {
            if (t->watcher_count >= t->watcher_cap) {
                t->watcher_cap *= 2;
                t->watchers     = realloc(t->watchers,
                                          t->watcher_cap * sizeof(int));
                t->watcher_refs = realloc(t->watcher_refs,
                                          t->watcher_cap * sizeof(Val));
            }
            int ref = ++vm->next_ref;
            t->watchers[t->watcher_count]     = p->pid;
            t->watcher_refs[t->watcher_count] = val_int(ref);
            t->watcher_count++;
            proc_push(p, val_int(ref));
        } else {
            proc_push(p, val_nil());
        }
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
        proc_die(vm, p, val_nil());
        return -1;

    /* ---- pattern matching ---- */
    case OP_MATCH_INT: {
        int64_t expected;
        memcpy(&expected, &p->code[p->pc], 8); p->pc += 8;
        if (!match_ok) break;
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
        memcpy(&idx, &p->code[p->pc], 4); p->pc += 4;
        if (!match_ok) break;
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
        if (!match_ok) break;
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
        if (!match_ok) break;
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
        memcpy(&addr, &p->code[p->pc], 4); p->pc += 4;
        if (!match_ok) {
            p->pc     = addr;
            match_ok  = 1;
                }
        break;
    }

    /* ---- string builtins ---- */
    case OP_STR_LEN: {
        Val s = proc_pop(p);
        if (val_tag(s) != TAG_STRING) { proc_push(p, val_nil()); break; }
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
        if (!tmp) { proc_push(p, val_nil()); break; }
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
        if (start < 0) start = 0;
        if (end > hs->len) end = hs->len;
        if (start >= end) { proc_push(p, val_string(p, "", 0)); break; }
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
            case OP_CCALL: {
        int pc_start = p->pc - 1;  /* save for rewind on would-block */
        int cfidx;
        memcpy(&cfidx, p->code + p->pc, 4); p->pc += 4;
        uint8_t nc = p->code[p->pc++];
        if (cfidx < 0 || cfidx >= vm->cfunc_count) {
            /* Invalid cfunc index — skip args, push nil */
            for (int i = 0; i < nc; i++) proc_pop(p);
            proc_push(p, val_nil());
            break;
        }
        if (nc > 64) {
            /* Too many args — skip, push nil */
            for (int i = 0; i < nc; i++) proc_pop(p);
            proc_push(p, val_nil());
            break;
        }
        Val args[64];
        for (int i = nc - 1; i >= 0; i--) args[i] = proc_pop(p);
                vm->current_proc = p;
        vm->last_wait_fd = -1;
        vm->last_wait_events = POLLIN;
        Val result = vm->cfuncs[cfidx].fn(vm, args, nc);
                /* Check for would-block → transition to I/O wait */
        if (val_is_symbol(result)) {
            uint32_t sidx = val_get_symbol(result);
            if (sidx < (uint32_t)vm->sym_count &&
                strcmp(vm->symbols[sidx], "would-block") == 0) {
                /* Restore args to stack so OP_CCALL can re-pop on retry */
                for (int i = 0; i < nc; i++)
                    proc_push(p, args[i]);
                p->state       = PROC_WAIT_IO;
                p->wait_fd     = vm->last_wait_fd;
                p->wait_events = vm->last_wait_events;
                p->pc          = pc_start;  /* rewind to re-execute OP_CCALL */
                return -1;
            }
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