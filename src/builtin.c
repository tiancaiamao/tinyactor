/* ============================================================
 * builtin.c — the OP_BUILTIN actor-primitive table
 *
 * The cold-path actor primitives (spawn / send / receive / monitor …) used
 * to be their own opcodes with a hand-spelled handler in vm.c's dispatch
 * loop. They now live here, behind a one-byte index: codegen emits
 * `OP_BUILTIN, idx` and the OP_BUILTIN handler in vm.c calls
 * builtin_table[idx]. Keeping them out of the loop is what makes them
 * "cold": each one is a full function that owns p->sp and p->pc for the
 * duration of the call, so the hot-path loop locals never have to be
 * threaded into the blocking paths (selective receive, recv_after, …).
 *
 * Contract with the OP_BUILTIN handler (src/vm.c):
 *   - On entry p->sp and p->pc are current: p->sp is the real stack top and
 *     p->pc points at this builtin's operands (the byte after the idx).
 *   - A builtin reads its operands from p->code starting at p->pc, advances
 *     p->pc past them, and pushes its own result through p->sp (proc_push /
 *     proc_pop). It never sees or touches the handler's C local `sp`.
 *   - Returning B_OK leaves p->pc at the next instruction; the handler
 *     reloads its local sp from p->sp and continues.
 *   - Returning B_SUSPEND means the proc blocked: the handler rewinds p->pc
 *     to the OP_BUILTIN instruction and yields, so the scheduler re-runs the
 *     whole instruction when it wakes the proc. The builtin is responsible
 *     for having stored the block state (PROC_WAIT_RECV) under mbox_lock and
 *     for leaving p->sp as the proc should resume with.
 *
 * These run at instruction granularity, not per-VM-tick, so the extra
 * indirect call and the p->sp/p->pc traffic are the price of the flat,
 * readable blocking paths; the instructions that stay opcodes (cons / car /
 * cdr / the type tests / arithmetic / divzero) are the ones where that
 * price is not worth paying.
 * ============================================================ */
#include "ta.h"

/* ================================================================
 * spawn — a fresh proc running fn_id, enqueued on the run queue
 * ================================================================ */
/* Frame setup shared by BUILTIN_SPAWN / BUILTIN_SPAWN_MAIN, mirroring
 * vm_spawn(): fp starts negative so local slots (fp+offset) stay inside the
 * stack, the header occupies fp-1..fp-4. */
static Proc *spawn_fn(VM *vm, int32_t fn_id) {
    Proc *np = proc_new(vm);
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

static BStatus spawn_fn_common(VM *vm, Proc *p, int set_main) {
    int32_t fn_id;
    memcpy(&fn_id, &p->code[p->pc], 4);
    p->pc += 4;

    Proc *np = spawn_fn(vm, fn_id);
    /* Only BUILTIN_SPAWN_MAIN (compiler-spawned main()) sets main_pid.
     * Regular spawn from user code never changes main_pid. */
    if (set_main)
        vm->main_pid = np->pid;
    proc_push(p, val_pid((uint32_t)np->pid));
    return B_OK;
}

static BStatus b_spawn(VM *vm, Proc *p) { return spawn_fn_common(vm, p, 0); }
static BStatus b_spawn_main(VM *vm, Proc *p) { return spawn_fn_common(vm, p, 1); }

/* ================================================================
 * spawn_clos — spawn a closure value taken from the stack
 * ================================================================ */
static BStatus b_spawn_clos(VM *vm, Proc *p) {
    Val clos_val = proc_pop(p);
    Proc *np = proc_new(vm);
    proc_ensure_heap(np);

    /* Copy the whole closure into the CHILD's heap. The child must never
     * hold pointers into the parent's heap: the parent may GC (moving
     * objects) or die (freeing its heap) independently. TAG_CLOS_ID
     * immediates pass through val_deep_copy untouched. Reserve room first:
     * the fresh arena is tiny, and the gate-closed copy cannot grow it. */
    proc_reserve_heap(np, val_calc_heap_size(clos_val));
    Val owned = val_deep_copy(np, clos_val);

    /* Set up frame: free vars at fp+0..fp+nfree-1, header at fp-1..fp-4.
     * `owned` and `clos` stay valid across the pushes: the pushes perform no
     * allocation (only proc_push), so the child's pending collection is not
     * honoured until its first real allocation, by which point `owned` is
     * rooted on the child's stack (#136). */
    int nfree = 0;
    HeapClosure *clos = NULL;
    if ((owned >> 48) == TAG_CLOS) {
        clos = val_as_clos(owned);
        nfree = clos->nfree;
    }
    np->sp = 0;
    for (int i = nfree - 1; i >= 0; i--)
        proc_push(np, clos->free[i]);
    proc_push(np, owned);           /* fp-1 */
    proc_push(np, val_int(-1));     /* fp-2: ret_pc sentinel */
    proc_push(np, val_int(0));      /* fp-3: old_fp */
    proc_push(np, val_int(np->sp)); /* fp-4: caller_sp */
    np->fp = -nfree;                /* fp+0 = first free var */

    if ((clos_val >> 48) == TAG_CLOS_ID)
        np->pc = np->fn_table[(int)(clos_val & 0xFFFFFFFFFFFFULL)];
    else {
        HeapClosure *c = val_as_clos(clos_val);
        np->pc = np->fn_table[c->entry];
    }
    runq_enqueue(vm, np->pid);
    proc_push(p, val_pid((uint32_t)np->pid));
    return B_OK;
}

/* ================================================================
 * send — fire-and-forget a message into a target's mailbox
 * ================================================================ */
static BStatus b_send(VM *vm, Proc *p) {
    Val pid_v = proc_pop(p); /* pid pushed last → on top */
    Val msg = proc_pop(p);   /* msg pushed first */
    /* The target pid comes from user code; bound it the same way proc_new
     * bounds the table, so a fabricated/stale pid cannot index past
     * procs[]. */
    uint32_t tpid = val_get_pid(pid_v);
    Proc *t = (tpid < (uint32_t)vm->procs_cap) ? vm->procs[tpid] : NULL;
    if (t && atomic_load(&t->state) != PROC_DEAD) {
        /* mbox_deliver serializes msg into a malloc'd fragment on the
         * sender's side and wakes the target under its mbox_lock if blocked
         * on recv (enqueue-at-most-once → Skynet invariant). That is plain
         * malloc, no arena allocation anywhere on the path (see frag_copy),
         * so nothing here reads p->sp. */
        mbox_deliver(vm, t, msg);
    }
    proc_push(p, val_nil()); /* send returns nil to keep stack balanced */
    return B_OK;
}

/* ================================================================
 * recv — pop the next mailbox message, blocking if empty
 * ================================================================ */
static BStatus b_recv(VM *vm, Proc *p) {
    (void)vm;
    pthread_mutex_lock(&p->mbox_lock);
    if (p->mbox_count == 0) {
        /* Store the block state under mbox_lock: mbox_deliver checks state
         * under the same lock, so it cannot fall into the check-then-block
         * window and strand the message (same invariant as recv_after). */
        atomic_store(&p->state, PROC_WAIT_RECV);
        pthread_mutex_unlock(&p->mbox_lock);
        return B_SUSPEND;
    }
    pthread_mutex_unlock(&p->mbox_lock);
    /* mbox_pop reserves room and deep-copies, both of which read p->sp
     * (proc_reserve_heap / the copy's allocation), so it gets the real top
     * — p->sp is already current here. */
    Val msg = mbox_pop(p);
    proc_push(p, msg);
    /* mbox_pop's deep copy runs with the gate closed (its worklist is
     * off-stack), so a request may be pending; the popped message is now
     * rooted, so it is safe to honour it — proc_push leaves p->sp at the
     * slot the drain scans. */
    proc_gc_drain(p);
    return B_OK;
}

/* ================================================================
 * recv_peek / recv_commit — selective receive
 * ================================================================ */
/* Peek the next mailbox fragment (without removing it), deep-copied onto
 * this proc's heap. The compiler stores it in a temp slot and runs pattern
 * code against it.
 * - If a fragment exists: push it and advance peek_index.
 * - If the mailbox is exhausted: block on recv. peek_index is preserved so a
 *   resumed scan (after a new message arrives) only inspects unseen
 *   messages — already-skipped fragments don't match the (immutable)
 *   patterns, so skipping them forever is correct, and they stay for a
 *   future receive. */
static BStatus b_recv_peek(VM *vm, Proc *p) {
    (void)vm;
    pthread_mutex_lock(&p->mbox_lock);
    if (p->peek_index < p->mbox_count) {
        MsgFragment *frag = p->mbox_frag_head;
        for (int i = 0; i < p->peek_index; i++)
            frag = frag->next;
        /* Reserve before the gate-closed copy; see mbox_pop. The collection
         * may run under mbox_lock — it takes no locks, so a concurrent
         * sender only waits, never deadlocks. p->sp must be current for it:
         * proc_reserve_heap sizes the request from it. */
        proc_reserve_heap(p, val_calc_heap_size(frag->root));
        Val msg = val_deep_copy(p, frag->root);
        p->peek_index++;
        pthread_mutex_unlock(&p->mbox_lock);
        proc_push(p, msg);
        /* The copied message is rooted now; honour any collection the
         * gate-closed deep copy requested (same as recv) — proc_push
         * published the stack top the drain scans. */
        proc_gc_drain(p);
        return B_OK;
    }
    /* Same invariant as recv: store the block state while holding mbox_lock
     * so a concurrent mbox_deliver cannot miss the wake and strand the
     * message. */
    atomic_store(&p->state, PROC_WAIT_RECV);
    pthread_mutex_unlock(&p->mbox_lock);
    return B_SUSPEND;
}

/* A pattern matched: drop the fragment we just peeked (at peek_index-1) from
 * the mailbox and reset the scan cursor. The matched message's heap copy was
 * already consumed by the pattern (bound to variables); the fragment itself
 * is freed here. */
static BStatus b_recv_commit(VM *vm, Proc *p) {
    (void)vm;
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
    return B_OK;
}

/* ================================================================
 * recv_after — wait up to `ms` for the next message, nil on timeout
 * ================================================================ */
/* recv_after(ms): wait up to ms for the next mailbox message. Message
 * available first -> pop and return it (FIFO, same as recv). Deadline passes
 * first -> return nil; the mailbox is untouched (Erlang/Gleam semantics: a
 * timeout never consumes messages). The atomic deadline tracks the armed
 * state: -1 = the ms operand is still on the stack (arm on this execution);
 * RECV_AFTER_EXPIRED = the scheduler's deadline scan already fired the
 * timeout while we were blocked — return nil without touching the mailbox
 * even if a message arrived after expiry; >= 0 = armed deadline, re-check
 * mbox/timeout. On block the handler rewinds p->pc so this builtin
 * re-executes when woken — by mbox_deliver (message wins, deadline cleared)
 * or by the scheduler's deadline scan (mailbox still empty -> nil). */
static BStatus b_recv_after(VM *vm, Proc *p) {
    if (atomic_load(&p->recv_deadline_ms) == RECV_AFTER_EXPIRED) {
        atomic_store(&p->recv_deadline_ms, -1);
        atomic_fetch_sub(&vm->recv_armed, 1);
        proc_push(p, val_nil());
        return B_OK;
    }
    if (atomic_load(&p->recv_deadline_ms) < 0) {
        Val ms_v = proc_pop(p);
        atomic_store(&p->recv_deadline_ms, net_now_ms() + val_get_int(ms_v));
        atomic_fetch_add(&vm->recv_armed, 1);
        vm_wake_poller(vm);
    }
    pthread_mutex_lock(&p->mbox_lock);
    if (p->mbox_count > 0) {
        pthread_mutex_unlock(&p->mbox_lock);
        atomic_store(&p->recv_deadline_ms, -1);
        atomic_fetch_sub(&vm->recv_armed, 1);
        /* See recv: mbox_pop reads p->sp before the message it returns is
         * on the stack, and the drain after it scans the stack, so p->sp is
         * current at both points. */
        Val msg = mbox_pop(p);
        proc_push(p, msg);
        proc_gc_drain(p); /* message rooted; see recv */
        return B_OK;
    }
    if (net_now_ms() >= atomic_load(&p->recv_deadline_ms)) {
        pthread_mutex_unlock(&p->mbox_lock);
        atomic_store(&p->recv_deadline_ms, -1);
        atomic_fetch_sub(&vm->recv_armed, 1);
        proc_push(p, val_nil());
        return B_OK;
    }
    /* Block. State is stored under mbox_lock so a concurrent mbox_deliver
     * (which checks state under the same lock) cannot fall into the
     * check-then-block window and strand the message. */
    atomic_store(&p->state, PROC_WAIT_RECV);
    pthread_mutex_unlock(&p->mbox_lock);
    return B_SUSPEND;
}

/* ================================================================
 * monitor — watch a target, DOWN on death, return the ref
 * ================================================================ */
static BStatus b_monitor(VM *vm, Proc *p) {
    Val pid_v = proc_pop(p);
    uint32_t tpid = val_get_pid(pid_v);
    int ref = ++vm->next_ref;
    int alive = 0;
    /* Fetch the target and mutate its watcher array under procs_lock:
     * proc_die walks the same array under this lock (strictly after setting
     * PROC_DEAD), so without the lock a concurrent death tears the realloc'd
     * array (issue #123). */
    pthread_mutex_lock(&vm->procs_lock);
    Proc *t = (tpid < (uint32_t)vm->procs_cap) ? vm->procs[tpid] : NULL;
    if (t && atomic_load(&t->state) != PROC_DEAD) {
        /* Normal path: join watchers, DOWN sent when target dies */
        if (t->watcher_count >= t->watcher_cap) {
            t->watcher_cap = t->watcher_cap ? t->watcher_cap * 2 : 4;
            t->watchers = realloc(t->watchers, t->watcher_cap * sizeof(int));
            t->watcher_refs = realloc(t->watcher_refs, t->watcher_cap * sizeof(Val));
        }
        t->watchers[t->watcher_count] = p->pid;
        t->watcher_refs[t->watcher_count] = val_int(ref);
        t->watcher_count++;
        alive = 1;
    }
    pthread_mutex_unlock(&vm->procs_lock);
    if (!alive) {
        /* Target already dead or nonexistent: deliver DOWN immediately. Only
         * THIS ref is at stake: we did not insert it, so proc_die's iteration
         * cannot produce a duplicate. The nested val_pair chain keeps each
         * intermediate pair only in a C local across the next allocation, so
         * no collection may run inside it: reserve its room first, then close
         * the gate. The reservation sizes itself from p->sp, so p->sp must be
         * current before it (the pop above left it so). */
        proc_reserve_heap(p, 4 * ta_heap_object_size(sizeof(HeapPair)));
        proc_gc_enter(p);
        int down_sym = vm_intern_symbol(vm, "DOWN");
        int noproc_sym = vm_intern_symbol(vm, "noproc");
        Val msg =
            val_pair(p, val_symbol((uint32_t)down_sym),
                     val_pair(p, val_int(ref),
                              val_pair(p, val_pid(tpid),
                                       val_pair(p, val_symbol((uint32_t)noproc_sym), val_nil()))));
        mbox_deliver(vm, p, msg);
        /* msg was serialized out; nothing is at risk. The reopen can drain,
         * i.e. collect on this stack, so the top it scans has to be the real
         * one. */
        proc_gc_reopen(p);
    }
    /* No double-check needed: if the target died after we released the lock,
     * proc_die's iteration - under the same lock, and only after PROC_DEAD is
     * set - necessarily observes our entry. The old racy re-check assumed an
     * unlocked insert that a concurrent death could skip (issue #123). */
    proc_push(p, val_int(ref));
    return B_OK;
}

/* The one-byte OP_BUILTIN operand indexes this table; the order is the
 * contract with BuiltinId in ta.h (and, once codegen emits OP_BUILTIN, with
 * the indices codegen.ta picks). */
const BuiltinFn builtin_table[BUILTIN_COUNT] = {
    [BUILTIN_SPAWN] = b_spawn,
    [BUILTIN_SPAWN_MAIN] = b_spawn_main,
    [BUILTIN_SPAWN_CLOS] = b_spawn_clos,
    [BUILTIN_SEND] = b_send,
    [BUILTIN_RECV] = b_recv,
    [BUILTIN_RECV_PEEK] = b_recv_peek,
    [BUILTIN_RECV_COMMIT] = b_recv_commit,
    [BUILTIN_RECV_AFTER] = b_recv_after,
    [BUILTIN_MONITOR] = b_monitor,
};