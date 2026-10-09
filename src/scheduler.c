/*
 * scheduler.c — process lifecycle, threads, run queue, mailbox
 *
 * Extracted from the original vm.c. All functions here were formerly
 * file-static in vm.c; they are now non-static and declared in ta.h.
 */

/*
 * Linux exposes fcntl()/O_NONBLOCK only under _POSIX_C_SOURCE with
 * -std=c99; macOS keeps BSD constants visible via _DARWIN_C_SOURCE.
 * _DEFAULT_SOURCE covers usleep(). Must precede any #include.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#define _DEFAULT_SOURCE /* expose POSIX usleep() under -std=c99 */

#include "ta.h"
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Forward declaration for internal scheduler functions */
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
static inline uint64_t frag_payload48(Val v) { return v & 0x0000FFFFFFFFFFFFULL; }

/* Build a NaN-boxed pointer value (tag | low48 ptr). */
static inline Val frag_box_ptr(uint16_t tag, void *ptr) {
    return ((uint64_t)tag << 48) | ((uint64_t)(uintptr_t)ptr & 0x0000FFFFFFFFFFFFULL);
}

/* Total bytes needed in data[] for a Val tree (each object 8-aligned). */
typedef struct {
    Val value;
} FragSizeTask;

static void frag_stack_grow(void **items, int *cap, int count, size_t size) {
    if (count < *cap)
        return;
    int next = *cap ? *cap * 2 : 64;
    void *grown = realloc(*items, (size_t)next * size);
    if (!grown)
        abort();
    *items = grown;
    *cap = next;
}

int frag_calc_size(Val root) {
    FragSizeTask *tasks = NULL;
    int count = 0, cap = 0;
    int total = 0;

    frag_stack_grow((void **)&tasks, &cap, count, sizeof(*tasks));
    tasks[count++].value = root;
    while (count > 0) {
        Val v = tasks[--count].value;
        uint16_t tag = val_tag(v);
        if (tag == TAG_PAIR) {
            HeapPair *src = (HeapPair *)(uintptr_t)frag_payload48(v);
            total += FRAG_ALIGN8(sizeof(HeapPair));
            frag_stack_grow((void **)&tasks, &cap, count, sizeof(*tasks));
            tasks[count++].value = src->cdr;
            frag_stack_grow((void **)&tasks, &cap, count, sizeof(*tasks));
            tasks[count++].value = src->car;
        } else if (tag == TAG_STRING) {
            HeapString *s = (HeapString *)(uintptr_t)frag_payload48(v);
            total += FRAG_ALIGN8(sizeof(HeapString) + s->len + 1);
        } else if (tag == TAG_BYTES) {
            HeapBytes *b = (HeapBytes *)(uintptr_t)frag_payload48(v);
            total += FRAG_ALIGN8(sizeof(HeapBytes) + b->len);
        } else if (tag == TAG_CLOS) {
            HeapClosure *c = (HeapClosure *)(uintptr_t)frag_payload48(v);
            total += FRAG_ALIGN8(sizeof(HeapClosure) + c->nfree * (int)sizeof(Val));
            for (int i = c->nfree - 1; i >= 0; i--) {
                frag_stack_grow((void **)&tasks, &cap, count, sizeof(*tasks));
                tasks[count++].value = c->free[i];
            }
        }
    }
    free(tasks);
    return total;
}

/* Copy a Val tree into fragment f's data[] (8-aligned placements).
 * Returns a new Val whose pointers address the fragment's data[]. */
typedef struct {
    int kind;
    Val value;
    void *dst;
} FragCopyTask;

#define FRAG_COPY_VALUE 0
#define FRAG_COPY_PAIR 1
#define FRAG_COPY_CLOSURE 2

Val frag_copy(MsgFragment *f, Val root) {
    FragCopyTask *tasks = NULL;
    Val *results = NULL;
    int task_count = 0, task_cap = 0;
    int result_count = 0, result_cap = 0;

    frag_stack_grow((void **)&tasks, &task_cap, task_count, sizeof(*tasks));
    tasks[task_count++] = (FragCopyTask){FRAG_COPY_VALUE, root, NULL};
    while (task_count > 0) {
        FragCopyTask task = tasks[--task_count];
        uint16_t tag = val_tag(task.value);
        if (task.kind == FRAG_COPY_PAIR) {
            Val cdr = results[--result_count];
            Val car = results[--result_count];
            f->size = FRAG_ALIGN8(f->size);
            HeapPair *dst = (HeapPair *)(f->data + f->size);
            f->size += sizeof(HeapPair);
            dst->hdr.type = HEAP_PAIR;
            dst->hdr.flags = 0;
            dst->car = car;
            dst->cdr = cdr;
            frag_stack_grow((void **)&results, &result_cap, result_count, sizeof(*results));
            results[result_count++] = frag_box_ptr(TAG_PAIR, dst);
            continue;
        }
        if (task.kind == FRAG_COPY_CLOSURE) {
            HeapClosure *dst = (HeapClosure *)task.dst;
            for (int i = dst->nfree - 1; i >= 0; i--)
                dst->free[i] = results[--result_count];
            frag_stack_grow((void **)&results, &result_cap, result_count, sizeof(*results));
            results[result_count++] = frag_box_ptr(TAG_CLOS, dst);
            continue;
        }
        if (tag == TAG_PAIR) {
            HeapPair *src = (HeapPair *)(uintptr_t)frag_payload48(task.value);
            frag_stack_grow((void **)&tasks, &task_cap, task_count, sizeof(*tasks));
            tasks[task_count++] = (FragCopyTask){FRAG_COPY_PAIR, task.value, NULL};
            frag_stack_grow((void **)&tasks, &task_cap, task_count, sizeof(*tasks));
            tasks[task_count++] = (FragCopyTask){FRAG_COPY_VALUE, src->cdr, NULL};
            frag_stack_grow((void **)&tasks, &task_cap, task_count, sizeof(*tasks));
            tasks[task_count++] = (FragCopyTask){FRAG_COPY_VALUE, src->car, NULL};
        } else if (tag == TAG_STRING) {
            HeapString *src = (HeapString *)(uintptr_t)frag_payload48(task.value);
            f->size = FRAG_ALIGN8(f->size);
            HeapString *dst = (HeapString *)(f->data + f->size);
            f->size += sizeof(HeapString) + src->len + 1;
            dst->hdr.type = HEAP_STRING;
            dst->hdr.flags = 0;
            dst->len = src->len;
            memcpy(dst->data, src->data, src->len);
            dst->data[src->len] = '\0';
            frag_stack_grow((void **)&results, &result_cap, result_count, sizeof(*results));
            results[result_count++] = frag_box_ptr(TAG_STRING, dst);
        } else if (tag == TAG_BYTES) {
            HeapBytes *src = (HeapBytes *)(uintptr_t)frag_payload48(task.value);
            f->size = FRAG_ALIGN8(f->size);
            HeapBytes *dst = (HeapBytes *)(f->data + f->size);
            f->size += sizeof(HeapBytes) + src->len;
            dst->hdr.type = HEAP_BYTES;
            dst->hdr.flags = 0;
            dst->len = src->len;
            memcpy(dst->data, src->data, src->len);
            frag_stack_grow((void **)&results, &result_cap, result_count, sizeof(*results));
            results[result_count++] = frag_box_ptr(TAG_BYTES, dst);
        } else if (tag == TAG_CLOS) {
            HeapClosure *src = (HeapClosure *)(uintptr_t)frag_payload48(task.value);
            f->size = FRAG_ALIGN8(f->size);
            HeapClosure *dst = (HeapClosure *)(f->data + f->size);
            f->size += sizeof(HeapClosure) + src->nfree * (int)sizeof(Val);
            dst->hdr.type = HEAP_CLOS;
            dst->hdr.flags = 0;
            dst->entry = src->entry;
            dst->nfree = src->nfree;
            frag_stack_grow((void **)&tasks, &task_cap, task_count, sizeof(*tasks));
            tasks[task_count++] = (FragCopyTask){FRAG_COPY_CLOSURE, task.value, dst};
            for (int i = src->nfree - 1; i >= 0; i--) {
                frag_stack_grow((void **)&tasks, &task_cap, task_count, sizeof(*tasks));
                tasks[task_count++] = (FragCopyTask){FRAG_COPY_VALUE, src->free[i], NULL};
            }
        } else {
            frag_stack_grow((void **)&results, &result_cap, result_count, sizeof(*results));
            results[result_count++] = task.value;
        }
    }
    Val result = results[0];
    free(tasks);
    free(results);
    return result;
}

/* ================================================================
 * Mailbox — fragment-based FIFO (thread-safe)
 * ================================================================ */

/* Serialize msg into a fresh fragment, append it to target's mailbox,
 * and wake the target if it is blocked on recv — all under the target's
 * mbox_lock so the WAIT_RECV->RUNNING transition + enqueue are atomic
 * w.r.t. concurrent senders. This guarantees a proc is enqueued at most
 * once (Skynet invariant: never two threads running the same proc). */
void mbox_deliver(VM *vm, Proc *target, Val msg) {
    int need = frag_calc_size(msg);
    MsgFragment *frag = (MsgFragment *)malloc(sizeof(MsgFragment) + need);
    if (!frag)
        return; /* OOM — message dropped */
    frag->next = NULL;
    frag->size = 0;
    frag->root = frag_copy(frag, msg);

    pthread_mutex_lock(&target->mbox_lock);
    if (atomic_load(&target->state) == PROC_DEAD) {
        pthread_mutex_unlock(&target->mbox_lock);
        free(frag);
        return;
    }
    if (target->mbox_frag_tail)
        target->mbox_frag_tail->next = frag;
    else
        target->mbox_frag_head = frag;
    target->mbox_frag_tail = frag;
    target->mbox_count++;
    if (atomic_load(&target->state) == PROC_WAIT_RECV) {
        atomic_store(&target->state, PROC_RUNNING);
        vm_wait_unregister(vm, target);
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
    if (!p->mbox_frag_head)
        p->mbox_frag_tail = NULL;
    p->mbox_count--;
    pthread_mutex_unlock(&p->mbox_lock);

    /* The message may exceed the arena's free space, and the gate-closed
     * deep copy below cannot grow the arena — reserve first (gate is open
     * here, so growth is a safe collection). */
    proc_reserve_heap(p, val_calc_heap_size(frag->root));
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
        int *new_q = malloc(new_cap * sizeof(int));
        int count = vm->rq_tail - vm->rq_head;
        for (int i = 0; i < count; i++)
            new_q[i] = vm->runq[(vm->rq_head + i) % vm->rq_cap];
        free(vm->runq);
        vm->runq = new_q;
        vm->rq_cap = new_cap;
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
    if (atomic_load(&vm->rq_count) == 0)
        return -1;
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
    p->pid = atomic_fetch_add(&vm->next_pid, 1);

    /* procs[] is a fixed-size table indexed by pid, and pids are never
     * reused, so a program that spawns more than procs_cap processes over
     * its lifetime would index past the end of the table. Fail loudly here
     * instead of silently corrupting the heap next to it (issue #129: an
     * unbounded spawn chain overflowed procs[] and surfaced as SIGSEGV /
     * "gc: unknown heap type 0"). */
    if (p->pid >= vm->procs_cap) {
        fprintf(stderr,
                "fatal: process table exhausted (pid %d >= cap %d) — "
                "lifetime spawn count is bounded by MAX_PROCS (TA_MAX_PROCS)\n",
                p->pid, vm->procs_cap);
        fflush(stderr);
        abort();
    }

    atomic_store(&p->state, PROC_RUNNING);

    /* procs[] is pre-allocated to procs_cap — never realloc'd; the pid
     * bound above is what keeps the index inside it */
    pthread_mutex_lock(&vm->procs_lock);
    vm->procs[p->pid] = p;
    vm->procs_count++;
    /* Publish the shared module pointers under procs_lock. */
    p->code = vm->code;
    p->fn_table = vm->fn_table;
    p->fn_count = vm->fn_count;
    pthread_mutex_unlock(&vm->procs_lock);
    atomic_fetch_add(&vm->active_procs, 1);

    /* execution context — heap lazily allocated on first use */
    p->mem_size = 0;
    p->mem = NULL;
    p->gc_to = NULL;
    p->heap_ptr = 0;
    p->sp = 0;
    p->fp = 0;
    p->pc = 0;
    p->gc_pending = 0;
    p->gc_trigger = 0;
    p->gc_gate = 0;
    /* chunk arena: the inline first chunk lives at the bottom of mem */
    p->gc_ck_head.next = NULL;
    p->gc_ck_head.used = 0;
    p->gc_ck_head.cap = TA_PROC_CHUNK0;
    p->gc_ck_cur = &p->gc_ck_head;

    /* mailbox — fragment list (starts empty; calloc zeroed the rest) */
    p->mbox_frag_head = NULL;
    p->mbox_frag_tail = NULL;
    p->mbox_count = 0;
    pthread_mutex_init(&p->mbox_lock, NULL);

    /* I/O wait state: no fd and no deadline until vm_watch_fd() sets one */
    p->wait_fd = -1;
    atomic_init(&p->wait_deadline_ms, -1);
    atomic_init(&p->recv_deadline_ms, -1);

    /* watchers — lazily allocated (NULL, 0) */
    p->watcher_cap = 0;
    p->watchers = NULL;
    p->watcher_refs = NULL;

    return p;
}

/* proc_free is provided externally or in vm_free implementation */

void proc_die(VM *vm, Proc *p, Val reason) {
    /* main() died abnormally → the host must exit non-zero. Set the flag at
     * proc_die entry, BEFORE the active_procs-- below: that decrement can
     * trip the "no live processes" stop path and let the scheduler loop
     * return while the crash report is still being printed, and the reader
     * would see main_crashed == 0 → exit 0 on a crashed main (raced with
     * fprintf to stderr; ~5% in crash tests). */
    if (p->pid == vm->main_pid && !val_is_nil(reason))
        atomic_store(&vm->main_crashed, 1);

    int was_wait_io = (atomic_load(&p->state) == PROC_WAIT_IO);
    atomic_store(&p->state, PROC_DEAD);
    vm_wait_unregister(vm, p);
    atomic_fetch_sub(&vm->active_procs, 1);

    /* Crash visibility (issue #28): a non-nil reason is an abnormal death
     * (div-zero, call-non-function, bad opcode, ...) — print a crash report
     * to stderr with pid, reason symbol, and a stack trace (leaf..root).
     * nil = normal exit (OP_RET root frame / OP_HALT): stay fully silent,
     * exactly as before. The stack walk reads p->mem, so this must run
     * before free(p->mem) below. */
    if (!val_is_nil(reason)) {
        if (p->mem != NULL) {
            int frames[64];
            int depth = vm_walk_stack(vm, p, frames, 64);
            fprintf(stderr, "** CRASH pid %d: '", p->pid);
            if (val_is_symbol(reason))
                fprintf(stderr, "%s", vm->symbols[val_get_symbol(reason)]);
            else
                fprintf(stderr, "?");
            fprintf(stderr, "\n");
            for (int i = 0; i < depth; i++) {
                const char *name = vm_fn_name(vm, frames[i]);
                fprintf(stderr, "   at %s (fn %d)\n", name ? name : "?", frames[i]);
            }
            fflush(stderr);
        }
        /* main_crashed flag itself is set at proc_die entry above */
    }

    /* Clear from procs[] table under procs_lock to avoid race with io_poller_thread */
    pthread_mutex_lock(&vm->procs_lock);
    vm->procs[p->pid] = NULL;
    vm->procs_count--;
    pthread_mutex_unlock(&vm->procs_lock);

    /* Stop VM when no live processes remain.
     * When main() exits, shut the VM down (Gleam/Go semantics: the VM's
     * lifetime is bound to main). Actors parked on external events
     * (WAIT_IO, or WAIT_RECV with nothing to wake them) are killed at
     * once — e.g. a gateway blocked in accept() used to hang the VM
     * forever after a test's main() had already printed its result.
     * Actors still in the runq get a short stall-bounded grace below so
     * already-sent messages drain before the force stop. */
    if (atomic_load(&vm->active_procs) == 0) {
        atomic_store(&vm->stop, 1);
        vm_wake_poller(vm);
        pthread_cond_broadcast(&vm->rq_cond);
    } else if (p->pid == vm->main_pid) {
        atomic_store(&vm->main_dead, 1);
        pthread_mutex_lock(&vm->wait_lock);
        Proc *waiting = vm->wait_head;
        vm->wait_head = NULL;
        while (waiting) {
            Proc *next = waiting->wait_next;
            waiting->wait_next = NULL;
            waiting->wait_registered = 0;
            waiting->wait_generation++;
            atomic_fetch_sub(&vm->wait_count, 1);
            waiting = next;
        }
        pthread_mutex_unlock(&vm->wait_lock);
        for (int i = 0; i < vm->procs_cap; i++) {
            Proc *q = vm->procs[i];
            int state = q ? atomic_load(&q->state) : PROC_DEAD;
            if (q && (state == PROC_WAIT_IO || state == PROC_WAIT_RECV)) {
                atomic_store(&q->state, PROC_DEAD);
                atomic_fetch_sub(&vm->active_procs, 1);
            }
        }
        if (atomic_load(&vm->active_procs) == 0)
            atomic_store(&vm->stop, 1);
        vm_wake_poller(vm);
        pthread_cond_broadcast(&vm->rq_cond);
    }
    if (was_wait_io && p->wait_fd >= 0) {
        close(p->wait_fd);
        p->wait_fd = -1;
    }

    pthread_mutex_lock(&vm->procs_lock);
    /* Walk watchers under procs_lock: the monitor builtin inserts entries
     * under the same lock, so a concurrently-arriving monitor cannot tear
     * the array
     * (issue #123). Invariant: PROC_DEAD is set before this loop, and the
     * loop is the only reader, so every entry inserted before a death is
     * delivered exactly one DOWN here.
     *
     * The ('DOWN ...) tree is built from several val_pair allocations whose
     * intermediate pairs live only in C locals, and `reason` sits in a C
     * local too, so no collection may run here: reserve the walk's room
     * (one 4-pair message per watcher), then close the gate for the whole
     * walk. (p's heap is freed a few lines below; the request, if any, is
     * dropped with it — no drain needed.) */
    proc_reserve_heap(p, p->watcher_count * 4 * ta_heap_object_size(sizeof(HeapPair)));
    proc_gc_enter(p);
    for (int i = 0; i < p->watcher_count; i++) {
        int wid = p->watchers[i];
        Proc *w = vm->procs[wid];
        if (!w || atomic_load(&w->state) == PROC_DEAD)
            continue;
        /* Build ('DOWN ref pid reason) on the CURRENT process p's heap
         * (p is not executing here → safe), then cross-heap-deliver
         * via mbox_deliver, which serializes into a malloc'd fragment
         * and wakes the watcher under its mbox_lock if blocked on recv. */
        int down_sym = vm_intern_symbol(vm, "DOWN");
        Val msg = val_pair(p, val_symbol((uint32_t)down_sym),
                           val_pair(p, p->watcher_refs[i],
                                    val_pair(p, val_pid(p->pid), val_pair(p, reason, val_nil()))));
        mbox_deliver(vm, w, msg);
    }
    proc_gc_leave(p);
    pthread_mutex_unlock(&vm->procs_lock);

    /* Free token vectors owned by this proc (ids are per-proc and opaque,
     * so no other thread can reference them once p is dead). */
    vm_free_proc_tokvecs(p);

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
     * concurrently in the monitor builtin accessing them. They are freed in
     * vm_free. */
    proc_chunk_reset(p); /* free the callback chunk chain (malloc'd chunks) */
    free(p->mem);
    p->mem = NULL;
    free(p->gc_to);
    p->gc_to = NULL;
    p->mem_size = 0;
    p->heap_ptr = 0;
    p->gc_pending = 0;

    /* Retire the Proc struct itself: watchers/watcher_refs may still be read
     * by a concurrent monitor builtin call, so their free is deferred to
     * vm_free (all threads joined). Without this list the struct would be
     * leaked. */
    pthread_mutex_lock(&vm->procs_lock);
    p->next_retired = vm->retired;
    vm->retired = p;
    pthread_mutex_unlock(&vm->procs_lock);
}

/* ================================================================
 * Public: spawn a process running fn_id
 *
 * set_main != 0 records vm->main_pid BEFORE the runq publish below:
 * a compiler-spawned main() can crash on its very first instruction,
 * and proc_die's `p->pid == vm->main_pid` check must already see the
 * flag — writing main_pid after this function returned raced the
 * worker taking the new proc off the runq (~5% of crash tests exited
 * 0 on a crashed main). The write happens-before the enqueue's
 * unlock, so any worker that sees the proc also sees main_pid. */
Proc *proc_new_frame(VM *vm, int fn_id, int set_main) {
    Proc *np = proc_new(vm);
    if (set_main)
        vm->main_pid = np->pid;
    proc_ensure_heap(np);
    np->fp = -4;
    np->sp = -8;
    proc_stack(np)[np->fp - 1] = val_nil();       /* closure  */
    proc_stack(np)[np->fp - 2] = val_int(-1);     /* ret_pc sentinel */
    proc_stack(np)[np->fp - 3] = val_int(0);      /* old_fp   */
    proc_stack(np)[np->fp - 4] = val_int(np->sp); /* caller_sp*/
    np->pc = np->fn_table[fn_id];
    runq_enqueue(vm, np->pid);
    return np;
}

int vm_spawn(VM *vm, int fn_id) { return proc_new_frame(vm, fn_id, 0)->pid; }

/* ================================================================
 * Scheduler
 * ================================================================ */
#define MAX_REDUCTIONS 1000

/* Register a published wait. Proc storage is retained until vm_free, so
 * poll snapshots can safely hold Proc pointers while a concurrent wake
 * removes the registration. */
static void wait_register(VM *vm, Proc *p) {
    int needs_wakeup;
    pthread_mutex_lock(&vm->wait_lock);
    if (!p->wait_registered) {
        p->wait_next = vm->wait_head;
        vm->wait_head = p;
        p->wait_registered = 1;
        p->wait_generation++;
        atomic_fetch_add(&vm->wait_count, 1);
    }
    needs_wakeup = atomic_load(&p->state) == PROC_WAIT_IO || atomic_load(&p->recv_deadline_ms) >= 0;
    pthread_mutex_unlock(&vm->wait_lock);
    if (needs_wakeup)
        vm_wake_poller(vm);
}

static void wait_unregister_locked(VM *vm, Proc *p) {
    if (p->wait_registered) {
        Proc **link = &vm->wait_head;
        while (*link && *link != p)
            link = &(*link)->wait_next;
        if (*link == p)
            *link = p->wait_next;
        p->wait_next = NULL;
        p->wait_registered = 0;
        p->wait_generation++;
        atomic_fetch_sub(&vm->wait_count, 1);
    }
}

static void wait_unregister(VM *vm, Proc *p) {
    pthread_mutex_lock(&vm->wait_lock);
    wait_unregister_locked(vm, p);
    pthread_mutex_unlock(&vm->wait_lock);
}

static void drain_wake_pipe(VM *vm) {
    char buf[64];
    while (read(vm->wake_pipe_r, buf, sizeof buf) > 0) {
    }
}

void vm_wait_register(VM *vm, Proc *p) { wait_register(vm, p); }

int vm_wait_count(VM *vm) { return atomic_load(&vm->wait_count); }

void vm_wait_unregister(VM *vm, Proc *p) {
    wait_unregister(vm, p);
    vm_wake_poller(vm);
}

static void *io_poller_thread(void *arg) {
    VM *vm = (VM *)arg;
    while (!atomic_load(&vm->stop)) {
        pthread_mutex_lock(&vm->wait_lock);
        int count = 1;
        for (Proc *p = vm->wait_head; p; p = p->wait_next)
            if (atomic_load(&p->state) == PROC_WAIT_IO)
                count++;
        struct pollfd *pfds = calloc((size_t)count, sizeof(*pfds));
        Proc **procs = calloc((size_t)count, sizeof(*procs));
        unsigned *generations = calloc((size_t)count, sizeof(*generations));
        if (!pfds || !procs || !generations) {
            free(pfds);
            free(procs);
            free(generations);
            pthread_mutex_unlock(&vm->wait_lock);
            usleep(1000);
            continue;
        }
        pfds[0].fd = vm->wake_pipe_r;
        pfds[0].events = POLLIN;
        int n = 1;
        int64_t deadline = timer_next_deadline_ms(vm);
        for (Proc *p = vm->wait_head; p; p = p->wait_next) {
            int state = atomic_load(&p->state);
            int64_t d = state == PROC_WAIT_IO     ? atomic_load(&p->wait_deadline_ms)
                        : state == PROC_WAIT_RECV ? atomic_load(&p->recv_deadline_ms)
                                                  : -1;
            if (d >= 0 && (deadline < 0 || d < deadline))
                deadline = d;
            if (state == PROC_WAIT_IO) {
                pfds[n].fd = p->wait_fd;
                pfds[n].events = p->wait_events;
                procs[n] = p;
                generations[n] = p->wait_generation;
                n++;
            }
        }
        pthread_mutex_unlock(&vm->wait_lock);

        int timeout = -1;
        if (deadline >= 0) {
            int64_t delta = deadline - net_now_ms();
            timeout = delta <= 0 ? 0 : delta > INT_MAX ? INT_MAX : (int)delta;
        }
        int result = poll(pfds, (nfds_t)n, timeout);
        int64_t now = net_now_ms();
        if (result > 0 && (pfds[0].revents & POLLIN))
            drain_wake_pipe(vm);

        for (int i = 1; i < n; i++) {
            Proc *p = procs[i];
            if (!p || !(pfds[i].revents & (POLLIN | POLLOUT | POLLERR | POLLHUP | POLLNVAL)))
                continue;
            pthread_mutex_lock(&vm->wait_lock);
            int waiting = p->wait_registered && p->wait_generation == generations[i] &&
                          atomic_load(&p->state) == PROC_WAIT_IO;
            if (waiting) {
                atomic_store(&p->state, PROC_RUNNING);
                wait_unregister_locked(vm, p);
                runq_enqueue(vm, p->pid);
            }
            pthread_mutex_unlock(&vm->wait_lock);
        }

        /* Expire registered waits only; recv-after keeps its mailbox lock
         * ordering so a message and timeout cannot enqueue the proc twice. */
        pthread_mutex_lock(&vm->wait_lock);
        Proc *p = vm->wait_head;
        while (p) {
            int state = atomic_load(&p->state);
            int64_t d = state == PROC_WAIT_IO     ? atomic_load(&p->wait_deadline_ms)
                        : state == PROC_WAIT_RECV ? atomic_load(&p->recv_deadline_ms)
                                                  : -1;
            if (d >= 0 && now >= d) {
                if (state == PROC_WAIT_RECV) {
                    pthread_mutex_unlock(&vm->wait_lock);
                    pthread_mutex_lock(&p->mbox_lock);
                    if (atomic_load(&p->state) == PROC_WAIT_RECV &&
                        atomic_load(&p->recv_deadline_ms) >= 0 &&
                        now >= atomic_load(&p->recv_deadline_ms)) {
                        atomic_store(&p->recv_deadline_ms, RECV_AFTER_EXPIRED);
                        atomic_store(&p->state, PROC_RUNNING);
                        pthread_mutex_unlock(&p->mbox_lock);
                        wait_unregister(vm, p);
                        runq_enqueue(vm, p->pid);
                    } else {
                        pthread_mutex_unlock(&p->mbox_lock);
                    }
                    pthread_mutex_lock(&vm->wait_lock);
                    p = vm->wait_head;
                    continue;
                } else if (state == PROC_WAIT_IO && p->wait_registered) {
                    atomic_store(&p->state, PROC_RUNNING);
                    wait_unregister_locked(vm, p);
                    runq_enqueue(vm, p->pid);
                    p = vm->wait_head;
                    continue;
                }
            }
            p = p->wait_next;
        }
        pthread_mutex_unlock(&vm->wait_lock);
        timer_fire_expired(vm, now);
        free(pfds);
        free(generations);
        free(procs);
    }
    pthread_mutex_lock(&vm->rq_lock);
    pthread_cond_broadcast(&vm->rq_cond);
    pthread_mutex_unlock(&vm->rq_lock);
    return NULL;
}

void vm_wake_poller(VM *vm) {
    if (vm->wake_pipe_w < 0)
        return;
    char b = 0;
    ssize_t n = write(vm->wake_pipe_w, &b, 1);
    (void)n;
}

/* io poller 生命周期，独立成对导出：轻量宿主（lispvm）只需要这一个
 * 事件驱动，不用 worker 池。 */
void vm_poller_start(VM *vm) {
    atomic_store(&vm->stop, 0);

    /* The wake pipe lets arm sites publish registrations/deadlines and lets
     * shutdown interrupt an idle poll. */
    int wp[2];
    if (pipe(wp) == 0) {
        fcntl(wp[0], F_SETFL, fcntl(wp[0], F_GETFL, 0) | O_NONBLOCK);
        fcntl(wp[1], F_SETFL, fcntl(wp[1], F_GETFL, 0) | O_NONBLOCK);
        vm->wake_pipe_r = wp[0];
        vm->wake_pipe_w = wp[1];
    } else {
        fprintf(stderr, "scheduler: failed to create poller wake pipe\n");
        abort();
    }

    if (pthread_create(&vm->io_thread, NULL, io_poller_thread, vm) != 0) {
        fprintf(stderr, "scheduler: failed to start io poller\n");
        abort();
    }
    vm->io_thread_started = 1;
}

void vm_poller_stop(VM *vm) {
    atomic_store(&vm->stop, 1);
    vm_wake_poller(vm);
    if (vm->io_thread_started) {
        pthread_join(vm->io_thread, NULL);
        vm->io_thread_started = 0;
    }
}
