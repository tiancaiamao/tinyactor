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
static void  mbox_deliver(VM *vm, Proc *target, Val msg);
static Val   mbox_pop(Proc *p);
static void  runq_enqueue(VM *vm, int pid);
static int   runq_trydequeue(VM *vm);
static Proc *proc_new(VM *vm);
static void  proc_die(VM *vm, Proc *p, Val reason);
static void  worker_loop(WorkerCtx *wc);
static void *worker_thread_entry(void *arg);
static void *io_poller_thread(void *arg);

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
    p->wait_fd     = fd;
    p->wait_events = events;
}

void vm_yield(VM *vm) {
    vm->yield_requested = 1;
}

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
 * Message fragments
 *
 * A fragment is a single malloc'd block holding a serialized copy
 * of a message's heap object tree. Because it lives in malloc'd
 * memory (never inside a process's fromspace), the GC neither
 * scans nor moves it — this is what makes cross-process send safe
 * under threading without touching gc.c.
 *
 * Layout inside data[]: each heap object is placed at an 8-byte
 * aligned offset. All pointers in the copied tree are rewritten to
 * point WITHIN the fragment, so val_deep_copy() can later traverse
 * frag->root and rebuild the tree on the receiver's own heap.
 * ================================================================ */

#define FRAG_ALIGN8(x) (((x) + 7) & ~7)

/* Low-48-bit payload extractor (val.c's version is file-static). */
static inline uint64_t frag_payload48(Val v) {
    return v & 0x0000FFFFFFFFFFFFULL;
}

/* Build a NaN-boxed pointer value (tag | low48 ptr). */
static inline Val frag_box_ptr(uint16_t tag, void *ptr) {
    return ((uint64_t)tag << 48) |
           ((uint64_t)(uintptr_t)ptr & 0x0000FFFFFFFFFFFFULL);
}

/* Total bytes needed in data[] for a Val tree (each object 8-aligned). */
static int frag_calc_size(Val v) {
    uint16_t tag = val_tag(v);
    if (tag == TAG_PAIR) {
        HeapPair *src = (HeapPair *)(uintptr_t)frag_payload48(v);
        return FRAG_ALIGN8(sizeof(HeapPair))
             + frag_calc_size(src->car)
             + frag_calc_size(src->cdr);
    }
    if (tag == TAG_STRING) {
        HeapString *s = (HeapString *)(uintptr_t)frag_payload48(v);
        return FRAG_ALIGN8(sizeof(HeapString) + s->len + 1);
    }
    if (tag == TAG_BYTES) {
        HeapBytes *b = (HeapBytes *)(uintptr_t)frag_payload48(v);
        return FRAG_ALIGN8(sizeof(HeapBytes) + b->len);
    }
    if (tag == TAG_CLOS) {
        HeapClosure *c = (HeapClosure *)(uintptr_t)frag_payload48(v);
        int sz = FRAG_ALIGN8(sizeof(HeapClosure) + c->nfree * (int)sizeof(Val));
        for (int i = 0; i < c->nfree; i++)
            sz += frag_calc_size(c->free[i]);
        return sz;
    }
    return 0; /* immediates (int, nil, bool, pid, sym, clos-id) */
}

/* Copy a Val tree into fragment f's data[] (8-aligned placements).
 * Returns a new Val whose pointers address the fragment's data[]. */
static Val frag_copy(MsgFragment *f, Val v) {
    uint16_t tag = val_tag(v);
    if (tag == TAG_PAIR) {
        HeapPair *src = (HeapPair *)(uintptr_t)frag_payload48(v);
        Val car = frag_copy(f, src->car);
        Val cdr = frag_copy(f, src->cdr);
        f->size = FRAG_ALIGN8(f->size);
        HeapPair *dst = (HeapPair *)(f->data + f->size);
        f->size += sizeof(HeapPair);
        dst->hdr.type  = HEAP_PAIR;
        dst->hdr.flags = 0;
        dst->car = car;
        dst->cdr = cdr;
        return frag_box_ptr(TAG_PAIR, dst);
    }
    if (tag == TAG_STRING) {
        HeapString *src = (HeapString *)(uintptr_t)frag_payload48(v);
        f->size = FRAG_ALIGN8(f->size);
        HeapString *dst = (HeapString *)(f->data + f->size);
        f->size += sizeof(HeapString) + src->len + 1;
        dst->hdr.type  = HEAP_STRING;
        dst->hdr.flags = 0;
        dst->len = src->len;
        memcpy(dst->data, src->data, src->len);
        dst->data[src->len] = '\0';
        return frag_box_ptr(TAG_STRING, dst);
    }
    if (tag == TAG_BYTES) {
        HeapBytes *src = (HeapBytes *)(uintptr_t)frag_payload48(v);
        f->size = FRAG_ALIGN8(f->size);
        HeapBytes *dst = (HeapBytes *)(f->data + f->size);
        f->size += sizeof(HeapBytes) + src->len;
        dst->hdr.type  = HEAP_BYTES;
        dst->hdr.flags = 0;
        dst->len = src->len;
        memcpy(dst->data, src->data, src->len);
        return frag_box_ptr(TAG_BYTES, dst);
    }
    if (tag == TAG_CLOS) {
        HeapClosure *src = (HeapClosure *)(uintptr_t)frag_payload48(v);
        f->size = FRAG_ALIGN8(f->size);
        HeapClosure *dst = (HeapClosure *)(f->data + f->size);
        f->size += sizeof(HeapClosure) + src->nfree * (int)sizeof(Val);
        dst->hdr.type  = HEAP_CLOS;
        dst->hdr.flags = 0;
        dst->entry = src->entry;
        dst->nfree = src->nfree;
        for (int i = 0; i < src->nfree; i++)
            dst->free[i] = frag_copy(f, src->free[i]);
        return frag_box_ptr(TAG_CLOS, dst);
    }
    return v; /* immediates */
}

/* ================================================================
 * Mailbox — fragment-based FIFO (thread-safe)
 * ================================================================ */

/* Serialize msg into a fresh fragment, append it to target's mailbox,
 * and wake the target if it is blocked on recv — all under the target's
 * mbox_lock so the WAIT_RECV->RUNNING transition + enqueue are atomic
 * w.r.t. concurrent senders. This guarantees a proc is enqueued at most
 * once (Skynet invariant: never two workers running the same proc). */
static void mbox_deliver(VM *vm, Proc *target, Val msg) {
    int need = frag_calc_size(msg);
    MsgFragment *frag = (MsgFragment *)malloc(sizeof(MsgFragment) + need);
    if (!frag) return; /* OOM — message dropped */
    frag->next = NULL;
    frag->size = 0;
    frag->root = frag_copy(frag, msg);

    pthread_mutex_lock(&target->mbox_lock);
    if (target->mbox_frag_tail) target->mbox_frag_tail->next = frag;
    else                        target->mbox_frag_head = frag;
    target->mbox_frag_tail = frag;
    target->mbox_count++;
    if (target->state == PROC_WAIT_RECV) {
        target->state = PROC_RUNNING;
        runq_enqueue(vm, target->pid);
    }
    pthread_mutex_unlock(&target->mbox_lock);
}

/* Detach the head fragment and rebuild its tree on p's own heap.
 * Caller guarantees p owns its execution context (no heap races). */
static Val mbox_pop(Proc *p) {
    pthread_mutex_lock(&p->mbox_lock);
    MsgFragment *frag = p->mbox_frag_head;
    p->mbox_frag_head = frag->next;
    if (!p->mbox_frag_head) p->mbox_frag_tail = NULL;
    p->mbox_count--;
    pthread_mutex_unlock(&p->mbox_lock);

    Val v = val_deep_copy(p, frag->root);
    free(frag);
    return v;
}

/* ================================================================
 * Run queue
 * ================================================================ */
static void runq_enqueue(VM *vm, int pid) {
    pthread_mutex_lock(&vm->rq_lock);
    if (vm->rq_tail - vm->rq_head >= vm->rq_cap) {
        int new_cap = vm->rq_cap * 2;
        int *new_q  = malloc(new_cap * sizeof(int));
        int count = vm->rq_tail - vm->rq_head;
        for (int i = 0; i < count; i++)
            new_q[i] = vm->runq[(vm->rq_head + i) % vm->rq_cap];
        free(vm->runq);
        vm->runq    = new_q;
        vm->rq_cap  = new_cap;
        vm->rq_head = 0;
        vm->rq_tail = count;
    }
    vm->runq[vm->rq_tail % vm->rq_cap] = pid;
    vm->rq_tail++;
    atomic_fetch_add(&vm->rq_count, 1);
    pthread_cond_signal(&vm->rq_cond);
    pthread_mutex_unlock(&vm->rq_lock);
}

static int runq_trydequeue(VM *vm) {
    if (atomic_load(&vm->rq_count) == 0) return -1;
    pthread_mutex_lock(&vm->rq_lock);
    if (atomic_load(&vm->rq_count) == 0) {
        pthread_mutex_unlock(&vm->rq_lock);
        return -1;
    }
    int pid = vm->runq[vm->rq_head % vm->rq_cap];
    vm->rq_head++;
    atomic_fetch_sub(&vm->rq_count, 1);
    pthread_mutex_unlock(&vm->rq_lock);
    return pid;
}

/* ================================================================
 * Process lifecycle
 * ================================================================ */
static Proc *proc_new(VM *vm) {
    Proc *p = calloc(1, sizeof(Proc));
    p->pid   = atomic_fetch_add(&vm->next_pid, 1);
    p->state = PROC_RUNNING;

    /* procs[] pre-allocated to MAX_PROCS — no realloc needed */
    vm->procs[p->pid] = p;
    vm->procs_count++;
    atomic_fetch_add(&vm->active_procs, 1);

        /* execution context */
    p->mem_size = 65536;
    p->mem      = calloc(1, p->mem_size);  /* zero-init for GC safety */
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

            /* mailbox — fragment list (starts empty; calloc zeroed the rest) */
    p->mbox_frag_head = NULL;
    p->mbox_frag_tail = NULL;
    p->mbox_count     = 0;
    pthread_mutex_init(&p->mbox_lock, NULL);

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
        atomic_fetch_sub(&vm->active_procs, 1);
        /* Stop VM when no live processes remain.
         * When main() exits, set flag so workers can drain runq first. */
    if (atomic_load(&vm->active_procs) == 0) {
        vm->stop = 1;
        pthread_cond_broadcast(&vm->rq_cond);
    } else if (p->pid == vm->main_pid) {
        vm->main_dead = 1;
        pthread_cond_broadcast(&vm->rq_cond);
    }
    if (was_wait_io && p->wait_fd >= 0) {
        close(p->wait_fd);
        p->wait_fd = -1;
    }

    for (int i = 0; i < p->watcher_count; i++) {
        int  wid = p->watchers[i];
        Proc *w  = vm->procs[wid];
        if (!w || w->state == PROC_DEAD) continue;
                        /* Build ('DOWN ref pid reason) on the CURRENT process p's heap
         * (p is owned by this worker → safe), then cross-heap-deliver
         * via mbox_deliver, which serializes into a malloc'd fragment
         * and wakes the watcher under its mbox_lock if blocked on recv. */
        int down_sym = vm_intern_symbol(vm, "DOWN");
        Val msg = val_pair(p,
            val_symbol((uint32_t)down_sym),
            val_pair(p,
                p->watcher_refs[i],
                val_pair(p,
                    val_pid(p->pid),
                    val_pair(p,
                        reason,
                        val_nil()))));
                mbox_deliver(vm, w, msg);
    }

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

/* Dedicated I/O poller thread (multi-thread mode). Collects all
 * PROC_WAIT_IO processes, calls poll(), and re-enqueues any whose
 * fds became ready. Runs concurrently with the worker threads so no
 * worker is ever blocked inside poll(). */
static void *io_poller_thread(void *arg) {
    VM *vm = (VM *)arg;
    while (!vm->stop) {
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

        if (nfds > 0) {
            poll(pfds, (nfds_t)nfds, 100);  /* 100ms timeout */
            for (int i = 0; i < nfds; i++) {
                if (pfds[i].revents & (POLLIN | POLLOUT | POLLERR | POLLHUP)) {
                    Proc *p = vm->procs[pids[i]];
                    if (p && p->state == PROC_WAIT_IO) {
                        p->state = PROC_RUNNING;
                        runq_enqueue(vm, p->pid);
                    }
                }
            }
        } else {
            usleep(1000);  /* no WAIT_IO actors; brief sleep */
        }
    }
    return NULL;
}

void vm_run(VM *vm) {
    atomic_store(&vm->active_procs, 1);
    atomic_store(&vm->busy_workers, 0);
    vm->stop = 0;

    if (vm->nworkers <= 1) {
        /* Single-thread degenerate mode */
        WorkerCtx wc = { .vm = vm, .current_proc = NULL, .thread_id = 0 };
        worker_loop(&wc);
        return;
    }

        /* Multi-thread mode: spawn the I/O poller thread + N workers */
    pthread_t io_thread;
    pthread_create(&io_thread, NULL, io_poller_thread, vm);

    vm->workers = malloc(vm->nworkers * sizeof(pthread_t));
    WorkerCtx *wctxs = malloc(vm->nworkers * sizeof(WorkerCtx));

    for (int i = 0; i < vm->nworkers; i++) {
        wctxs[i].vm = vm;
        wctxs[i].current_proc = NULL;
        wctxs[i].thread_id = i;
        pthread_create(&vm->workers[i], NULL, worker_thread_entry, &wctxs[i]);
    }

    for (int i = 0; i < vm->nworkers; i++)
        pthread_join(vm->workers[i], NULL);

    /* Workers have stopped; signal the poller and join it */
    vm->stop = 1;
    pthread_join(io_thread, NULL);

    free(wctxs);
}

/* pthread entry trampoline: hand the WorkerCtx to worker_loop. */
static void *worker_thread_entry(void *arg) {
    worker_loop((WorkerCtx *)arg);
    return NULL;
}

static void worker_loop(WorkerCtx *wc) {
    VM  *vm    = wc->vm;
    int  multi = (vm->nworkers > 1);
    int  stall = 0;
    for (;;) {
        if (vm->stop) break;

                        /* Phase 1: run all ready processes */
        int ran = 0;
        int pid;
        /* Mark ourselves busy BEFORE dequeuing to close the race window
         * where rq_count==0 && busy_workers==0 is falsely observed. */
        atomic_fetch_add(&vm->busy_workers, 1);
        while ((pid = runq_trydequeue(vm)) >= 0) {
            if (vm->stop) break;
            Proc *p = vm->procs[pid];
            if (!p || p->state != PROC_RUNNING) continue;
            ran = 1;
            tls_current_proc   = p;
            wc->current_proc   = p;
            for (int r = 0; r < MAX_REDUCTIONS; r++) {
                if (vm_step(vm, p) != 0) break;
            }
            if (p->state == PROC_RUNNING)
                runq_enqueue(vm, p->pid);
        }
        atomic_fetch_sub(&vm->busy_workers, 1);

        /* Stall detection: only count when NOTHING ran in the entire
         * inner loop iteration (runq empty, no progress).  A long-running
         * computation that re-enqueues itself is NOT a stall. */
                if (ran)
            stall = 0;
        else {
            stall++;
            /* When main() has exited, use a short grace period (200
             * iterations ≈ 200ms) so spawned actors can drain their
             * messages before we force-stop. */
            int stall_limit = vm->main_dead ? 200 : 10000;
                                    if (stall > stall_limit) {
                for (int i = 0; i < vm->procs_cap; i++) {
                    Proc *q = vm->procs[i];
                    if (q && q->state == PROC_RUNNING)
                        q->state = PROC_DEAD;
                }
                tls_current_proc = NULL;
                wc->current_proc = NULL;
                if (multi) {
                    /* Signal all other workers to stop too */
                    atomic_store(&vm->active_procs, 0);
                    vm->stop = 1;
                    pthread_cond_broadcast(&vm->rq_cond);
                }
                return;
            }
        }

        /* ---- Single-thread Phase 2 (unchanged) ---- */
        if (!multi) {
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

                        if (nfds == 0 || vm->main_dead) {
                /* No ready processes and no I/O waits → done.
                 * Also break when main() has exited — remaining I/O
                 * processes (e.g. server loops) are detached and should
                 * not keep the VM alive. */
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
            continue;
        }

        /* ---- Multi-thread Phase 2 ----
         * runq was empty for this worker. If no live procs remain
         * anywhere, the whole VM is quiescent → stop everyone. */
        if (atomic_load(&vm->active_procs) == 0) {
            vm->stop = 1;
            pthread_cond_broadcast(&vm->rq_cond);
            break;
        }

                /* Deadlock detection: runq empty + no busy worker + no
         * WAIT_IO actors → all remaining live actors are WAIT_RECV
         * (waiting for a message that can never arrive) → exit.
         * Any WAIT_IO actor is being handled by the poller thread,
         * so that is NOT a deadlock. */
        if (atomic_load(&vm->rq_count) == 0 &&
            atomic_load(&vm->busy_workers) == 0) {
            int has_wait_io = 0;
            for (int i = 0; i < vm->procs_cap; i++) {
                if (vm->procs[i] && vm->procs[i]->state == PROC_WAIT_IO) {
                    has_wait_io = 1;
                    break;
                }
            }
            if (!has_wait_io) {
                vm->stop = 1;
                pthread_cond_broadcast(&vm->rq_cond);
                break;
            }
        }

        /* I/O polling is handled by the dedicated poller thread; it
         * will wake us by enqueuing ready procs. Brief sleep to avoid
         * a busy spin. */
        usleep(1000);
    }
    tls_current_proc = NULL;
    wc->current_proc = NULL;
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
        if (val_is_nil(v)) proc_push(p, val_nil());
        else proc_push(p, val_get_car(v));
        break;
    }
    case OP_CDR: {
        Val v = proc_pop(p);
        if (val_is_nil(v)) proc_push(p, val_nil());
        else proc_push(p, val_get_cdr(v));
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
                        if ((closure_val >> 48) != TAG_CLOS && (closure_val >> 48) != TAG_CLOS_ID) {
            fprintf(stderr, "error: cannot call non-function value (tag=0x%04llx, raw=0x%llx, pc=%d, nargs=%d)\n",
                    (unsigned long long)(closure_val >> 48), (unsigned long long)closure_val, p->pc-4, nargs);
            p->state = PROC_DEAD;
            return -1;
        }
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
                                if ((closure_val >> 48) != TAG_CLOS && (closure_val >> 48) != TAG_CLOS_ID) {
            fprintf(stderr, "error: cannot call non-function value\n");
            p->state = PROC_DEAD;
            return -1;
        }
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

    case OP_ENTER: {
        /* Reserve stack space for local variables.
         * Pushes nslots nil values so that GC sees safe values
         * and the parent's stack is not overwritten. */
        int32_t nslots;
        memcpy(&nslots, &p->code[p->pc], 4); p->pc += 4;
        for (int i = 0; i < nslots; i++)
            proc_push(p, val_nil());
        break;
    }

        /* ---- actor primitives ---- */
                                                                                                                                case OP_SPAWN:
                                case OP_SPAWN_MAIN: {
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
            /* mbox_deliver serializes msg into a malloc'd fragment on the
             * sender's side and wakes the target under its mbox_lock if
             * blocked on recv (enqueue-at-most-once → Skynet invariant). */
            mbox_deliver(vm, t, msg);
        }
        proc_push(p, val_nil());   /* send returns nil to keep stack balanced */
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
            for (int i = 0; i < p->peek_index; i++) frag = frag->next;
            Val msg = val_deep_copy(p, frag->root);
            p->peek_index++;
            pthread_mutex_unlock(&p->mbox_lock);
            proc_push(p, msg);
        } else {
            pthread_mutex_unlock(&p->mbox_lock);
            p->pc--;                /* re-execute OP_RECV_PEEK on wake */
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
            if (!p->mbox_frag_head) p->mbox_frag_tail = NULL;
            free(frag);
        } else {
            MsgFragment *prev = p->mbox_frag_head;
            for (int i = 0; i < target - 1; i++) prev = prev->next;
            MsgFragment *frag = prev->next;
            prev->next = frag->next;
            if (frag == p->mbox_frag_tail) p->mbox_frag_tail = prev;
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
        int pc_start = p->pc - 1;  /* save for rewind on yield */
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
                tls_current_proc = p;
        vm->yield_requested = 0;
        Val result = vm->cfuncs[cfidx].fn(vm, args, nc);
                /* C function requested yield — suspend for I/O wait.
                 * wait_fd/wait_events were set via vm_watch_fd(). */
        if (vm->yield_requested) {
            for (int i = 0; i < nc; i++)
                proc_push(p, args[i]);
            p->state = PROC_WAIT_IO;
            p->pc    = pc_start;  /* rewind to re-execute OP_CCALL */
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