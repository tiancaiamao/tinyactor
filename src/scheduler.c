/*
 * scheduler.c — process lifecycle, threads, run queue, mailbox
 *
 * Extracted from the original vm.c. All functions here were formerly
 * file-static in vm.c; they are now non-static and declared in ta.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include <limits.h>
#include "ta.h"

/* Forward declarations for internal scheduler functions */
static void  worker_loop(WorkerCtx *wc);
static void *worker_thread_entry(void *arg);
static void *io_poller_thread(void *arg);

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
int frag_calc_size(Val v) {
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
Val frag_copy(MsgFragment *f, Val v) {
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
void mbox_deliver(VM *vm, Proc *target, Val msg) {
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
    if (atomic_load(&target->state) == PROC_WAIT_RECV) {
        atomic_store(&target->state, PROC_RUNNING);
        runq_enqueue(vm, target->pid);
    }
    pthread_mutex_unlock(&target->mbox_lock);
}

/* Detach the head fragment and rebuild its tree on p's own heap.
 * Caller guarantees p owns its execution context (no heap races). */
Val mbox_pop(Proc *p) {
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
void runq_enqueue(VM *vm, int pid) {
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

int runq_trydequeue(VM *vm) {
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
Proc *proc_new(VM *vm) {
    Proc *p = calloc(1, sizeof(Proc));
    p->pid   = atomic_fetch_add(&vm->next_pid, 1);
            atomic_store(&p->state, PROC_RUNNING);

    /* procs[] pre-allocated to MAX_PROCS — no realloc needed */
    pthread_mutex_lock(&vm->procs_lock);
    vm->procs[p->pid] = p;
    vm->procs_count++;
    pthread_mutex_unlock(&vm->procs_lock);
    atomic_fetch_add(&vm->active_procs, 1);

        /* execution context — heap lazily allocated on first use */
    p->mem_size = 0;
    p->mem      = NULL;
    p->gc_to    = NULL;
    p->heap_ptr = 0;
    p->sp       = 0;
    p->fp       = 0;
    p->pc       = 0;
    p->gc_root_count = 0;
    p->gc_roots      = NULL;
    p->gc_roots_cap  = 0;

    /* shared bytecode */
    p->code     = vm->code;
    p->fn_table = vm->fn_table;
    p->fn_count = vm->fn_count;

            /* mailbox — fragment list (starts empty; calloc zeroed the rest) */
    p->mbox_frag_head = NULL;
    p->mbox_frag_tail = NULL;
    p->mbox_count     = 0;
    pthread_mutex_init(&p->mbox_lock, NULL);

    /* watchers — lazily allocated (NULL, 0) */
    p->watcher_cap  = 0;
    p->watchers     = NULL;
    p->watcher_refs = NULL;

    return p;

}

/* proc_free is provided externally or in vm_free implementation */

void proc_die(VM *vm, Proc *p, Val reason) {
    int was_wait_io = (atomic_load(&p->state) == PROC_WAIT_IO);
        atomic_store(&p->state, PROC_DEAD);
        atomic_fetch_sub(&vm->active_procs, 1);

    /* Clear from procs[] table under procs_lock to avoid race with io_poller_thread */
    pthread_mutex_lock(&vm->procs_lock);
    vm->procs[p->pid] = NULL;
    vm->procs_count--;
    pthread_mutex_unlock(&vm->procs_lock);

        /* Stop VM when no live processes remain.
         * When main() exits, set flag so workers can drain runq first. */
    if (atomic_load(&vm->active_procs) == 0) {
        atomic_store(&vm->stop, 1);
        pthread_cond_broadcast(&vm->rq_cond);
        } else if (p->pid == vm->main_pid) {
        atomic_store(&vm->main_dead, 1);
        pthread_cond_broadcast(&vm->rq_cond);
    }
    if (was_wait_io && p->wait_fd >= 0) {
        close(p->wait_fd);
        p->wait_fd = -1;
    }

    pthread_mutex_lock(&vm->procs_lock);
        for (int i = 0; i < p->watcher_count; i++) {
        int  wid = p->watchers[i];
        Proc *w  = vm->procs[wid];
        if (!w || atomic_load(&w->state) == PROC_DEAD) continue;
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
    pthread_mutex_unlock(&vm->procs_lock);

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

        /* Release heap memory now that DOWN messages have been sent.
     * watchers/watcher_refs are NOT freed here — another thread may be
     * concurrently in OP_MONITOR accessing them. They are freed in vm_free. */
    free(p->mem);
    p->mem = NULL;
    free(p->gc_to);
    p->gc_to = NULL;
    free(p->gc_roots);
    p->gc_roots = NULL;
    p->gc_roots_cap = 0;
    p->gc_root_count = 0;
    p->mem_size = 0;
    p->heap_ptr = 0;
}

/* ================================================================
 * Public: spawn a process running fn_id
 * ================================================================ */
int vm_spawn(VM *vm, int fn_id) {
    Proc *np  = proc_new(vm);
    proc_ensure_heap(np);
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
    while (!atomic_load(&vm->stop)) {
        struct pollfd pfds[1024];
        int           pids[1024];
        int           nfds = 0;

        /* Collect fds under procs_lock to avoid race with proc_new/proc_die */
        pthread_mutex_lock(&vm->procs_lock);
        for (int i = 0; i < vm->procs_cap && nfds < 1024; i++) {
            Proc *p = vm->procs[i];
            if (p && atomic_load(&p->state) == PROC_WAIT_IO) {
                pfds[nfds].fd      = p->wait_fd;
                pfds[nfds].events  = p->wait_events;
                pfds[nfds].revents = 0;
                pids[nfds]         = p->pid;
                nfds++;
            }
        }
        pthread_mutex_unlock(&vm->procs_lock);

        if (nfds > 0) {
            poll(pfds, (nfds_t)nfds, 100);  /* 100ms timeout */

            /* Wake processes whose fds are ready - need procs_lock for safety */
            pthread_mutex_lock(&vm->procs_lock);
            for (int i = 0; i < nfds; i++) {
                if (pfds[i].revents & (POLLIN | POLLOUT | POLLERR | POLLHUP)) {
                    Proc *p = vm->procs[pids[i]];
                    if (p && atomic_load(&p->state) == PROC_WAIT_IO) {
                        atomic_store(&p->state, PROC_RUNNING);
                        runq_enqueue(vm, p->pid);
                    }
                }
            }
            pthread_mutex_unlock(&vm->procs_lock);
        } else {
            usleep(1000);  /* no WAIT_IO actors; brief sleep */
        }
    }
    return NULL;
}

void vm_run(VM *vm) {
        atomic_store(&vm->active_procs, 1);
    atomic_store(&vm->busy_workers, 0);
    atomic_store(&vm->stop, 0);

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
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 1 << 25);  /* 32 MiB */
        pthread_create(&vm->workers[i], &attr, worker_thread_entry, &wctxs[i]);
        pthread_attr_destroy(&attr);
    }

    for (int i = 0; i < vm->nworkers; i++)
        pthread_join(vm->workers[i], NULL);

        /* Workers have stopped; signal the poller and join it */
    atomic_store(&vm->stop, 1);
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
        if (atomic_load(&vm->stop)) break;

                        /* Phase 1: run all ready processes */
        int ran = 0;
        int pid;
        /* Mark ourselves busy BEFORE dequeuing to close the race window
         * where rq_count==0 && busy_workers==0 is falsely observed. */
        atomic_fetch_add(&vm->busy_workers, 1);
        while ((pid = runq_trydequeue(vm)) >= 0) {
                        if (atomic_load(&vm->stop)) break;
            pthread_mutex_lock(&vm->procs_lock);
            Proc *p = vm->procs[pid];
            pthread_mutex_unlock(&vm->procs_lock);
            if (!p || atomic_load(&p->state) != PROC_RUNNING) continue;
            ran = 1;
            tls_current_proc   = p;
            wc->current_proc   = p;
            for (int r = 0; r < MAX_REDUCTIONS; r++) {
                if (vm_step(vm, p) != 0) break;
            }
            if (atomic_load(&p->state) == PROC_RUNNING)
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
            /* When main() has exited, use a short grace period (10000
             * iterations ≈ 10s) so spawned actors can drain their
             * messages before we force-stop. Increased from 200 to avoid
             * race conditions with I/O-bound actors in single-thread mode. */
                        int stall_limit = atomic_load(&vm->main_dead) ? 10000 : 10000;
                                                                        if (stall > stall_limit) {
                for (int i = 0; i < vm->procs_cap; i++) {
                    Proc *q = vm->procs[i];
                    if (q && (q->state == PROC_RUNNING || q->state == PROC_WAIT_RECV))
                        q->state = PROC_DEAD;
                }
                tls_current_proc = NULL;
                wc->current_proc = NULL;
                if (multi) {
                    /* Signal all other workers to stop too */
                    pthread_cond_broadcast(&vm->rq_cond);
                }
                atomic_store(&vm->active_procs, 0);
                vm->stop = 1;
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

                                                if (atomic_load(&vm->active_procs) == 0) {
                /* All processes have exited — the VM is quiescent.
                 * Don't break on !nfds alone: processes blocked on
                 * recv (WAIT_RECV) are still alive and may be woken
                 * by messages from other processes.  Breaking here
                 * would orphan them.  The stall-detection path above
                 * handles the case where main() has exited but I/O
                 * processes linger (stall_limit=200). */
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
