/* ============================================================
 * builtin.c — actor 原语的宿主 cfunc 实现
 *
 * 这些函数不再有专用 opcode：编译期按裸名解析成 TAG_NATIVE，运行期走
 * CALL → do_ccall → 这里的普通 cfunc 协议（实参在 a[0..n-1]，结果按值
 * 返回）。do_ccall 已为调用关上 GC 门并挂上 chunk arena，所以深拷得到的
 * Val 直接返回即可，由调用方收敛进堆；这里不做 proc_push/proc_pop，也不
 * 碰解释器局部变量。
 *
 * 挂起的原语（recv 家族）用 vm_yield_recv：它必须在 p->mbox_lock 内调用，
 * do_ccall 随后在同一把锁里发布恢复点、置 PROC_WAIT_RECV 并解锁。这样
 * mbox_deliver（同锁）要么看到空邮箱之前的阻塞态，要么看到消息已经入队，
 * 不会丢唤醒。net 等 I/O 仍用 vm_yield（TA_YIELD_IO / poller）。
 * ============================================================ */
#include "ta.h"

static Proc *current_proc(void) { return tls_current_proc; }

static Val resource_die(Proc *p) {
    /* 收口违规不在 cfunc 里 proc_die：do_ccall 统一发布现场并隔离死。 */
    p->cfunc_die = "resource";
    return val_nil();
}

/* ================================================================
 * self / spawn / send — 不挂起
 * ================================================================ */
Val cfunc_self(VM *vm, Val *a, int n) {
    (void)vm;
    (void)a;
    (void)n;
    return val_pid((uint32_t)current_proc()->pid);
}

Val cfunc_spawn(VM *vm, Val *a, int n) {
    (void)n;
    Proc *p = current_proc();
    Proc *np;
    Val owned;
    int fid;
    if (proc_spawn_begin(vm, a[0], &np, &owned, &fid) != 0)
        return resource_die(p);
    proc_frame_init(vm, np, fid, owned); /* 建帧即压闭包入 fn 槽成根 */
    runq_enqueue(vm, np->pid);
    return val_pid((uint32_t)np->pid);
}

Val cfunc_send(VM *vm, Val *a, int n) {
    (void)n;
    uint32_t tpid = val_get_pid(a[0]);
    Proc *t = vm_proc_lookup(vm, (int)tpid);
    if (t && atomic_load(&t->state) != PROC_DEAD && mbox_deliver(vm, t, a[1]) != 0)
        return resource_die(current_proc());
    return val_nil();
}

/* ================================================================
 * recv / recv_after — 邮箱为空则在持锁窗口内挂起
 * ================================================================ */
Val cfunc_recv(VM *vm, Val *a, int n) {
    (void)a;
    (void)n;
    Proc *p = current_proc();
    pthread_mutex_lock(&p->mbox_lock);
    if (p->mbox_count == 0) {
        vm_yield_recv(vm); /* 锁留给 do_ccall 收口；本调用的结果会被丢弃 */
        return val_nil();
    }
    pthread_mutex_unlock(&p->mbox_lock);
    return mbox_pop_cfunc(p);
}

Val cfunc_recv_after(VM *vm, Val *a, int n) {
    (void)n;
    Proc *p = current_proc();

    if (atomic_load(&p->recv_deadline_ms) == RECV_AFTER_EXPIRED) {
        atomic_store(&p->recv_deadline_ms, -1);
        atomic_fetch_sub(&vm->recv_armed, 1);
        return val_nil();
    }
    if (atomic_load(&p->recv_deadline_ms) < 0) {
        atomic_store(&p->recv_deadline_ms, mono_now_ms() + val_get_int(a[0]));
        atomic_fetch_add(&vm->recv_armed, 1);
    }

    pthread_mutex_lock(&p->mbox_lock);
    if (p->mbox_count > 0) {
        pthread_mutex_unlock(&p->mbox_lock);
        atomic_store(&p->recv_deadline_ms, -1);
        atomic_fetch_sub(&vm->recv_armed, 1);
        return mbox_pop_cfunc(p);
    }
    if (mono_now_ms() >= atomic_load(&p->recv_deadline_ms)) {
        pthread_mutex_unlock(&p->mbox_lock);
        atomic_store(&p->recv_deadline_ms, -1);
        atomic_fetch_sub(&vm->recv_armed, 1);
        vm_wait_unregister(vm, p);
        return val_nil();
    }
    vm_yield_recv(vm); /* 持锁挂起：唤醒后重执行同一条 CALL，deadline 已 armed */
    return val_nil();
}

/* ================================================================
 * recv_peek / recv_commit — selective receive 的扫描游标
 * ================================================================ */
Val cfunc_recv_peek(VM *vm, Val *a, int n) {
    (void)a;
    (void)n;
    Proc *p = current_proc();
    pthread_mutex_lock(&p->mbox_lock);
    if (p->peek_index >= p->mbox_count) {
        vm_yield_recv(vm); /* peek_index 保留：唤醒后只扫新消息 */
        return val_nil();
    }
    MsgFragment *frag = p->mbox_frag_head;
    for (int i = 0; i < p->peek_index; i++)
        frag = frag->next;
    /* peek 禁令（docs/c-resource-design.md §4）：深拷不弹出会让 actor 堆和
     * 邮箱里各有两份 owner==self，GC 兜底会关掉排队中的 payload。 */
    if (val_has_resource(frag->root)) {
        pthread_mutex_unlock(&p->mbox_lock);
        return resource_die(p);
    }
    /* do_ccall 已关门且挂了 arena，深拷直接落在 chunk 上；无须（也不能在
     * 这里）proc_reserve_heap —— 它会要求 GC 门是开的。 */
    Val msg = val_deep_copy(p, frag->root, NULL);
    p->peek_index++;
    pthread_mutex_unlock(&p->mbox_lock);
    return msg;
}

Val cfunc_recv_commit(VM *vm, Val *a, int n) {
    (void)vm;
    (void)a;
    (void)n;
    Proc *p = current_proc();
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
    return val_nil();
}

/* ================================================================
 * monitor — 目标死亡时投递 DOWN，返回 ref
 * ================================================================ */
Val cfunc_monitor(VM *vm, Val *a, int n) {
    (void)n;
    Proc *p = current_proc();
    uint32_t tpid = val_get_pid(a[0]);
    int ref = atomic_fetch_add(&vm->next_ref, 1) + 1;
    int alive = 0;
    /* procs_lock 同时护住 watcher 数组与 proc_die 的 PROC_DEAD 发布。 */
    pthread_mutex_lock(&vm->procs_lock);
    Proc *t = (tpid < (uint32_t)vm->procs_cap) ? vm->procs[tpid] : NULL;
    if (t && atomic_load(&t->state) != PROC_DEAD) {
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
        /* 目标已死：只给这个 ref 补一条 DOWN。arena 已关门，DOWN 的节点
         * 落在 chunk 上；消息经 frag_copy 序列化入队，之后 arena 丢弃即可。 */
        Val noproc = val_symbol((uint32_t)vm_intern_symbol(vm, "noproc"));
        Val msg = down_msg_build(vm, p, val_int(ref), val_pid(tpid), noproc);
        mbox_deliver(vm, p, msg);
    }
    return val_int(ref);
}

/* 裸名注册：compile.ta 的 bare_cfunc 白名单必须与这张表逐项对应。 */
void vm_register_actor_primitives(VM *vm) {
    struct {
        const char *name;
        Val (*fn)(VM *vm, Val *args, int nargs);
        int nargs;
    } fns[] = {
        {"self", cfunc_self, 0},
        {"spawn", cfunc_spawn, 1},
        {"send", cfunc_send, 2},
        {"recv", cfunc_recv, 0},
        {"recv_after", cfunc_recv_after, 1},
        {"recv_peek", cfunc_recv_peek, 0},
        {"recv_commit", cfunc_recv_commit, 0},
        {"monitor", cfunc_monitor, 1},
    };
    for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++)
        vm_register(vm, fns[i].name, fns[i].fn, fns[i].nargs);
}