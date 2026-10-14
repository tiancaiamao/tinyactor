/* ============================================================
 * builtin.c — 共享的 actor 原语处理器表（冷路径）
 *
 * 旧 ISA 时代这里曾是 spawn/send/recv 等一整套带 p->pc 操作数协议的
 * opcode 处理器；lispvm 替换后，spawn/send/recv 由 do_builtin（src/vm.c）
 * 的 LB_* switch 原生实现，本表只剩 vm.c 借道复用的四个处理器：
 * monitor / recv_peek / recv_commit / recv_after。
 *
 * 与调用方（do_builtin）的约定：
 *   - 实参全在栈上（vm.c 先发布 p->sp、再把 acc flush 上栈），结果压栈，
 *     调用方 proc_pop 回 acc。处理器只经 proc_push/proc_pop 触栈，
 *     不看调用方的 C 局部 sp。
 *   - B_OK：结果已在栈顶，p->sp 为调用方继续执行的正确现场。
 *   - B_SUSPEND：proc 已阻塞——处理器在 p->mbox_lock 下查完阻塞条件、
 *     持锁返回、不动 p->state；vm.c 在同一锁窗内发布恢复点（LState）并置
 *     PROC_WAIT_RECV 后解锁，唤醒方（mbox_deliver 同锁）看到的阻塞态与
 *     恢复点互为一体。
 *
 * 这些处理器跑在指令粒度而非每 VM tick，多一次间接调用是冷路径可读性的
 * 价格；留在 opcode 里的（cons/car/cdr/类型测试/算术/除零）是付不起这个
 * 价的热指令。
 * ============================================================ */
#include "ta.h"

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
    /* Return B_SUSPEND still holding mbox_lock so the handler (do_builtin
     * in vm.c) can publish the resume point + PROC_WAIT_RECV atomically with
     * the empty-mailbox check above (a concurrent mbox_deliver serializes on
     * this lock). */
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
 * mbox/timeout. On block the handler publishes the resume point (LState) so
 * this builtin re-executes when woken — by mbox_deliver (message wins,
 * deadline cleared) or by the scheduler's deadline scan (mailbox still empty
 * -> nil). */
static BStatus b_recv_after(VM *vm, Proc *p) {
    if (atomic_load(&p->recv_deadline_ms) == RECV_AFTER_EXPIRED) {
        atomic_store(&p->recv_deadline_ms, -1);
        atomic_fetch_sub(&vm->recv_armed, 1);
        proc_push(p, val_nil());
        return B_OK;
    }
    if (atomic_load(&p->recv_deadline_ms) < 0) {
        Val ms_v = proc_pop(p);
        atomic_store(&p->recv_deadline_ms, mono_now_ms() + val_get_int(ms_v));
        atomic_fetch_add(&vm->recv_armed, 1);
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
    if (mono_now_ms() >= atomic_load(&p->recv_deadline_ms)) {
        pthread_mutex_unlock(&p->mbox_lock);
        atomic_store(&p->recv_deadline_ms, -1);
        atomic_fetch_sub(&vm->recv_armed, 1);
        vm_wait_unregister(vm, p);
        proc_push(p, val_nil());
        return B_OK;
    }
    /* Block. Return B_SUSPEND still holding mbox_lock: the handler
     * (do_builtin in vm.c) publishes the resume point (LState) +
     * PROC_WAIT_RECV under it, so a concurrent mbox_deliver (which checks
     * state under the same lock) can neither strand the message nor wake the
     * proc before the resume point lands. */
    return B_SUSPEND;
}

/* ================================================================
 * monitor — watch a target, DOWN on death, return the ref
 * ================================================================ */
static BStatus b_monitor(VM *vm, Proc *p) {
    Val pid_v = proc_pop(p);
    uint32_t tpid = val_get_pid(pid_v);
    int ref = atomic_fetch_add(&vm->next_ref, 1) + 1;
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

/* builtin_table is indexed by BuiltinId in ta.h — the table's internal
 * layout only. The compiled instruction is the three-word
 * OP_BUILTIN,id,n (compile.ta emit3); its id is compile.ta's B_* space,
 * which do_builtin in src/vm.c maps via the LB_* switch — the ordering
 * here is not the compiled id space. */
const BuiltinFn builtin_table[BUILTIN_COUNT] = {
    [BUILTIN_RECV_PEEK] = b_recv_peek,
    [BUILTIN_RECV_COMMIT] = b_recv_commit,
    [BUILTIN_RECV_AFTER] = b_recv_after,
    [BUILTIN_MONITOR] = b_monitor,
};