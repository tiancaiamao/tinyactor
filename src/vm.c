/*
 * vm.c — .tabc 解释器本体：opcode 消费、指令循环、内建派发、cfunc 调用
 * （名字→下标的解析在装载侧 link_unit，见 main.c）
 *
 * 宿主是 main.c（CLI / .tabc 装载 / 调度泵）：装载器写图像词流，
 * 本文件只读消费（见下方“装载侧接缝” extern 块）。当前进程与 VM 一律走
 * 参数（run_proc / 宏显式带 p、vm），不用文件全局——多 worker 并发跑不同
 * proc 时全局别名必然竞态。
 */

#include "ta.h"
#include "ta_inline.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Thread-local current process — set by the host run loop (tavm run_proc)
 * while a proc executes. */
__thread Proc *tls_current_proc = NULL;

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

void vm_yield(VM *vm) {
    (void)vm;
    /* Per-proc flag: multiple worker threads share one VM and call C
     * functions concurrently; a shared flag on VM would race across
     * threads (a spurious yield in one proc, or a lost yield in another). */
    Proc *p = tls_current_proc;
    if (p)
        p->yield_requested = 1;
}

void vm_die(VM *vm, const char *reason) {
    /* Same per-proc discipline as vm_yield (see above): sets a per-proc
     * flag instead of touching shared VM state — multiple worker threads
     * share one VM and call C functions concurrently. The cfunc return
     * path (do_ccall) checks yield_requested when the C function
     * returns. */
    Proc *p = tls_current_proc;
    if (p) {
        p->die_requested = 1;
        p->die_reason = val_symbol(vm_intern_symbol(vm, reason));
    }
}
/* ================================================================
 * Stack walking & function-name resolution
 *
 * Shared by the sampling profiler (prof.c) and the crash report
 * (proc_die in scheduler.c). The frame graph is image-specific: tavm
 * registers vm->walk_stack at image load (install_frame_hooks); the old
 * default p->mem frame-chain walker died with the TA opcode VM (#283).
 * No hook (image never loaded) → depth 0.
 * ================================================================ */
int vm_walk_stack(const VM *vm, const Proc *p, int *out, int max_depth) {
    if (vm->walk_stack)
        return vm->walk_stack(vm, p, out, max_depth);
    (void)p;
    (void)out;
    (void)max_depth;
    return 0;
}

/* fn_id -> fn name (.tabc v2 name table), or NULL if unknown (v1 module).
 * The caller may fall back to "fn#<id>" when this returns NULL. */
const char *vm_fn_name(const VM *vm, int fid) {
    if (vm->fn_names && fid >= 0 && fid < vm->fn_names_count && vm->fn_names[fid])
        return vm->fn_names[fid];
    return NULL;
}

/* op_builtin 派发 id（compile.ta B_* 的镜像，两边必须同步改）：
 * 已知内建编译期固化整数索引，这里 O(1) switch 派发 —— 名字永不进热路径 */
enum {
    LB_SELF = 0,
    LB_SPAWN,
    LB_SEND,
    LB_RECV,
    LB_MONITOR,
    LB_RECV_AFTER,
    LB_RECV_PEEK,
    LB_RECV_COMMIT,
};

/* ================================================================
 * 解释器内部：报错出口、返回码、栈宏、内建派发
 * ================================================================ */

/* 进程级报错出口（ta.h 声明）：宿主与解释器共用一个，报完即退。 */
_Noreturn void fatal(const char *msg) {
    fprintf(stderr, "tavm: %s\n", msg);
    exit(1);
}

/* 运行期类型/算术错误的进程隔离 die（vm.c 同款：'divzero / 'arithtype /
 * 'cartype / 'cdrtype）：只死当前 proc，monitor/link 传播、其余进程继续
 * 全部走共享 proc_die。仅限 run_proc 内使用 —— 中途 die 要先发布 pc/sp
 * （GC 与根扫描要一致状态），再清理解释器栈返回 R_DIED。 */
#define PROC_DIE_ISOLATED(reason)                                                                  \
    do {                                                                                           \
        int die_sym = vm_intern_symbol(vm, reason);                                                \
        st->pc = (int)(pc - W);                                                                    \
        st->base = base; /* 栈行走要的现场发布在 proc_die 前（lisp_walk_stack 用）*/ \
        st->cargs = cargs;                                                                         \
        p->sp = -(int)(vend - vsp);                                                                \
        proc_die(vm, p, val_symbol((uint32_t)die_sym));                                            \
        return R_DIED;                                                                             \
    } while (0)

/* ---- 栈：解释器深度 sp 与 p->sp 的映射 ----
 *
 * TA 栈向下长：p->sp = -sp。slot i（0 起底）固定位于
 *   mem + mem_size - (i + 1) * sizeof(Val)
 * 即 index -(i+1)：栈底在最浅处（index -1），后压的更深。地址只取决于 i，
 * 与 sp 无关 —— 帧内 base+k 寻址因此与原实现完全一致。sp 只决定多少 slot
 * 是活的：GC 扫 [p->sp, 0) = slot 0..sp-1，恰好是本解释器的全部活值
 * （常量区已不在栈上，见 g_consts）。
 * 宏显式带 proc 参数：当前进程不做文件全局，多 worker 并发不竞态。 */
#define SP_SET(proc, v)                                                                            \
    do {                                                                                           \
        sp = (v);                                                                                  \
        (proc)->sp = -(int)sp;                                                                     \
    } while (0)
#define SP_ADJ(proc, d) SP_SET(proc, (sp) + (d))
#define LSTK(proc, i) (*(Val *)((proc)->mem + (proc)->mem_size - ((int)(i) + 1) * (int)sizeof(Val)))

/* TAG_NATIVE：可调用值，载荷 = vm->cfuncs 下标（link_unit 装载期按名解析
 * 一次，带点名先 dlopen 自动加载）；CALL 期直接 vm->cfuncs[cf] 调用。 */
#define TAG_NATIVE 0xFFC1

/* ---- op_builtin 主体：actor 原语八条，放主循环外，别撑大派发热路径 ----
 *
 * 专用轻协议：前 n-1 参在栈顶（a1 = LSTK(p, sp-n+1) .. a(n-1) = LSTK(p, sp-1)），
 * 末参在 acc，无 fn 槽 —— 被调对象是编译期固化的整数 id（compile.ta B_*
 * 的镜像），switch 直接派发，零按名查找。结果写 *acc；tail 位置无需特殊
 * 处理（RET 会把 sp 收回 base）。零参/单参指令完全不动栈。
 * 返回非 0 = recv 阻塞：恢复点/栈已发布进 st，调用方直接 return R_BLOCKED。
 * LSTK 读 p 的栈：p 即本函数参数，LSTK/SP_* 宏显式带 p，无文件全局。 */
static int do_builtin(VM *vm, Proc *p, LState *st, Val *acc, long id, long n, long pc, long base,
                      long cbase, long cargs, long sp) {
    switch (id) {
    case LB_SELF: /* self () -> pid */
        if (n != 0)
            fatal("arity mismatch");
        *acc = val_pid((uint32_t)p->pid);
        break;
    case LB_SPAWN: { /* spawn (clos) -> pid，clos 在 acc */
        if (n != 1)
            fatal("arity mismatch");
        Val clos = *acc;
        long fid;
        if (val_tag(clos) == TAG_CLOS)
            fid = (long)((HeapClosure *)(uintptr_t)lpayload(clos))->entry;
        else if (val_tag(clos) == TAG_CLOS_ID)
            fid = (long)lpayload(clos);
        else
            fatal("spawn on non-closure");
        Proc *np = proc_new(vm);
        if (!np)
            fatal("proc_new failed");
        proc_ensure_heap(np);
        /* 闭包整体深拷入子堆：子绝不持父堆指针（父可独立 GC/死）。
         * 先 reserve：新 arena 很小，关门拷贝不能增长它（同 b_spawn_clos）。*/
        proc_reserve_heap(np, val_calc_heap_size(clos));
        Val owned = val_deep_copy(np, clos);
        proc_frame_init(vm, np, fid, owned); /* 建帧即压闭包入 fn 槽成根 */
        runq_enqueue(vm, np->pid);
        *acc = val_pid((uint32_t)np->pid); /* 无栈参，sp 不动 */
        break;
    }
    case LB_SEND: { /* send (pid msg) -> nil：pid 在栈顶，msg 在 acc */
        if (n != 2)
            fatal("arity mismatch");
        uint32_t tpid = val_get_pid(LSTK(p, sp - 1));
        Proc *t = vm_proc_lookup(vm, (int)tpid);
        if (t && atomic_load(&t->state) != PROC_DEAD)
            mbox_deliver(vm, t, *acc);
        p->sp = -(long)(sp - 1); /* 弹栈上的 pid（等价 SP_SET(p, sp-1)） */
        *acc = val_nil();
        break;
    }
    case LB_RECV: { /* recv () -> msg | 阻塞 */
        if (n != 0)
            fatal("arity mismatch");
        pthread_mutex_lock(&p->mbox_lock);
        if (p->mbox_count == 0) {
            /* 阻塞：本条不动栈，pc 不前进；先发布恢复点/栈，再置 WAIT_RECV
             * （与 TA OP_BUILTIN 同一顺序），唤醒后重执行本条。 */
            st->pc = pc;
            st->base = base;
            st->cbase = cbase;
            st->sp = sp;
            st->cargs =
                cargs; /* 当前帧实参数（do_builtin 只见 builtin 实参，帧 cargs 由调用方传入） */
            atomic_store(&p->state, PROC_WAIT_RECV);
            vm_wait_register(vm, p);
            pthread_mutex_unlock(&p->mbox_lock);
            return 1;
        }
        pthread_mutex_unlock(&p->mbox_lock);
        { /* mbox_pop 的深拷贝关门分配会记 gc_pending，
           * 先把消息压栈生根再 drain —— acc 是 C 局部，不是根集，
           * 带着未生根的 acc 收集 = GC 搬移后读悬垂指针（car 变 nil）。*/
            Val m = mbox_pop(p);
            proc_push(p, m);
            SP_ADJ(p, 1);
            proc_gc_drain(p);
            *acc = LSTK(p, sp - 1); /* 收集可能搬移，从栈重读 */
            SP_ADJ(p, -1);
        }
        break;
    }
    case LB_MONITOR: { /* monitor (pid) -> ref；pid 在 acc，永不阻塞 */
        if (n != 1)
            fatal("arity mismatch");
        /* 对接 builtin_table：TA builtin 从 p->sp 栈顶 proc_pop 取参，
         * 本 VM 协议末参在 acc —— 先发布栈顶再把 acc flush 上栈（TA 协议
         * 参数全在栈上），调完把结果从栈取回 acc，栈深复原。
         * b_monitor 恒 B_OK（死目标立即投递 DOWN，不挂起），无恢复点问题。 */
        p->sp = -(long)sp;
        proc_push(p, *acc);
        if (builtin_table[BUILTIN_MONITOR](vm, p) != B_OK)
            fatal("monitor must not suspend");
        *acc = proc_pop(p);
        break;
    }
    case LB_RECV_AFTER: { /* recv_after (ms) -> msg | nil：ms 在 acc。
        借道共享 b_recv_after（TA 协议：参全在栈、结果压栈、可挂起）。 */
        if (n != 1)
            fatal("arity mismatch");
        p->sp = -(long)sp;
        proc_push(p, *acc);
        if (builtin_table[BUILTIN_RECV_AFTER](vm, p) == B_SUSPEND) {
            /* b_recv_after 持 mbox_lock 返回 B_SUSPEND：在其锁窗内发布
             * 恢复点并置 WAIT_RECV（宿主 OP_BUILTIN 同款），登记后解锁。
             * 唤醒（投递或 poller deadline scan）重执行本条指令；重入时
             * acc=nil，ms 已存 recv_deadline_ms，走已 armed 路径。 */
            st->pc = pc;
            st->base = base;
            st->cbase = cbase;
            st->sp = sp;
            st->cargs =
                cargs; /* 当前帧实参数（do_builtin 只见 builtin 实参，帧 cargs 由调用方传入） */
            atomic_store(&p->state, PROC_WAIT_RECV);
            vm_wait_register(vm, p);
            pthread_mutex_unlock(&p->mbox_lock);
            return 1;
        }
        *acc = proc_pop(p);
        break;
    }
    case LB_RECV_PEEK: { /* recv_peek () -> msg | 阻塞。借道共享 b_recv_peek：
        窥视扫描游标处的消息（不消费）；游标耗尽则挂起，唤醒后重执行本
        条指令，从保留的游标继续 —— 选择性接收的跳过语义全在游标。 */
        if (n != 0)
            fatal("arity mismatch");
        p->sp = -(long)sp;
        if (builtin_table[BUILTIN_RECV_PEEK](vm, p) == B_SUSPEND) {
            /* b_recv_peek 持 mbox_lock 返回 B_SUSPEND：在其锁窗内发布恢复
             * 点并置 WAIT_RECV（LB_RECV_AFTER 同款），登记后解锁。唤醒
             * （投递）重执行本条 —— peek_index 未动，从原游标继续。 */
            st->pc = pc;
            st->base = base;
            st->cbase = cbase;
            st->sp = sp;
            st->cargs =
                cargs; /* 当前帧实参数（do_builtin 只见 builtin 实参，帧 cargs 由调用方传入） */
            atomic_store(&p->state, PROC_WAIT_RECV);
            vm_wait_register(vm, p);
            pthread_mutex_unlock(&p->mbox_lock);
            return 1;
        }
        *acc = proc_pop(p);
        break;
    }
    case LB_RECV_COMMIT: { /* recv_commit () -> nil：摘除游标处消息，游标归零。
        恒 B_OK（已验 peek 成功在先），无挂起路径。 */
        if (n != 0)
            fatal("arity mismatch");
        p->sp = -(long)sp;
        if (builtin_table[BUILTIN_RECV_COMMIT](vm, p) != B_OK)
            fatal("recv_commit must not suspend");
        *acc = val_nil();
        break;
    }
    default:
        fatal("unknown builtin");
    }
    return 0;
}

/* ---- op_call 的 TAG_NATIVE 分支：cfunc 调用协议（主循环外，do_builtin 同款）----
 *
 * 入口态：实参已 flush 上栈（末参在栈顶，槽 sp-1），fn 槽在 sp-n；cf = fv
 * 载荷（link_unit 装载期解析好的 cfunc 下标）。关门 + in_ccall 窗口协议
 * 照抄 cfunc 窗口协议：窗口内回调的分配走 chunk arena、不触发 moving
 * GC，返回后 converge 把 chunk 结果收敛进 heap，否则结果 Val 在下次 GC 后
 * 悬垂。
 * 返回 0：*acc = 结果，实参已弹、栈深回到调用点并发布 p->sp（调用方需
 * SP_REFRESH_PTRS 重物化）；返回非 0：三段非阻塞 cfunc（net.connect/read…）
 * 请求重入 —— pc 留在本指令起点、撤回入口 flush 的 acc（重执行时重压），
 * 完整解释器态进 LState，WAIT_IO 登记，调用方直接 return R_BLOCKED。 */
static int do_ccall(VM *vm, Proc *p, LState *st, Val *acc, long cf, long n, long base, long pc,
                    long cbase, long cargs, long sp) {
    if (cf >= vm->cfunc_count)
        fatal("bad cfunc index");
    /* nargs==-1=声明的变参（net.connect 可选 timeout），个数由 cfunc 自校
     * 验；宿主 VM 运行期不查 arity，此处只对固定参保留保险 */
    if (vm->cfuncs[cf].nargs >= 0 && vm->cfuncs[cf].nargs != (int)(n - 1))
        fatal("arity mismatch");
    /* 实参按下标逐个拷进 C 数组 —— 不能取 &LSTK(p, sp-n+1) 当基址：
     * 栈向低地址增长，C 数组方向（地址递增）与 slot 序（索引递增、
     * 地址递减）相反，args[1] 会读到 fn 槽（bug：str.concat 恒空串）。*/
    Val cargv[64]; /* 实参值数组（cargs 名被帧实参数参数占用） */
    if (n - 1 > 64)
        fatal("too many arguments");
    for (int ai = 0; ai < (int)(n - 1); ai++)
        cargv[ai] = LSTK(p, sp - n + 1 + ai);
    proc_gc_enter(p); /* gc_enter 记 lo：调用方已 SP_PUB，p->sp 即入口栈深 */
    p->in_ccall = 1;
    p->yield_requested = 0;
    Val r = vm->cfuncs[cf].fn(vm, cargv, (int)(n - 1));
    if (p->yield_requested) {
        p->yield_requested = 0;
        p->in_ccall = 0;
        proc_chunk_reset(p);
        proc_gc_leave(p);
        st->acc = *acc;
        st->has_acc = 1;
        st->pc = pc;
        st->base = base;
        st->cbase = cbase;
        st->cargs = cargs; /* 当前帧实参数（CALL 重执行时会重设，发布只为 walker 现场一致） */
        p->sp = -(int)(sp - 1); /* 撤回入口 flush 的 acc：回到指令入口 sp，重执行时重压 */
        st->sp = sp - 1;
        atomic_store(&p->state, PROC_WAIT_IO);
        vm_wait_register(vm, p);
        return 1;
    }
    p->in_ccall = 0;
    proc_chunk_converge(p, &r); /* gate 内收敛 chunk 结果（gate 内栈未动） */
    long s = sp - n; /* 实参已消费（gate 仍关，无收集风险）。尾调 native
                      * 无需特判：cont 留在帧内原位（块底恒在其上），结果
                      * 落 fn 槽，pc+=2 落 trailing IRET，帧在 IRET 收口。 */
    p->sp = -(int)s;
    LSTK(p, s) = r; /* 结果入栈作为根：帧区内直接写，reserve 已盖 */
    s += 1;         /* reopen 前 r 必须在根集内 */
    p->sp = -(int)s;
    proc_gc_reopen(p);     /* 补偿被关门压下的 GC 请求，
                            * 否则纯 ccall 工作负载（json/str 解析）请求永不清、堆只增不收 */
    *acc = LSTK(p, s - 1); /* 收集可能搬移对象，从栈重读（不能直接用 r） */
    s -= 1;
    p->sp = -(int)s;
    return 0;
}

int run_proc(VM *vm, Proc *p, LState *st) {
    tls_current_proc = p;
    /* 图像态（词流 / fn 表 / 常量表）：装载期写入 vm->*，run 期间只读。
     * 提为局部别名，热循环寻址与文件全局时代完全同形（W 不加 const：
     * pc/cbase 是 long*，指向词流内）。 */
    long *W = vm->words;
    const long *fn_entry = vm->fn_entry, *fn_args = vm->fn_args, *fn_maxd = vm->fn_maxd;
    const Val *g_consts = vm->g_consts;
    long base, cargs;
    /* pc/cbase 是指针（W + 偏移）：操作数寻址 *pc/pc[1] 免去 W 装载；
     * 发布（st->pc/st->cbase）时收回偏移 pc - W。 */
    long *pc, *cbase;
    Val acc;
    /* 栈深度指针化（run_proc 内的"寄存器"）：LSTK 每次访问都要装载
     * mem/mem_size 再乘加，vbase/vsp 把槽访问缩成一次解引用。指针只在
     * 本函数内活 —— 出 VM 边界（yield 保存 / do_builtin / 分配 grow）时
     * 用 SP_REFRESH_PTRS() 从 p 重新物化。 */
    Val *vend, *vsp, *vbase;
#define SP_REFRESH_PTRS()                                                                          \
    do {                                                                                           \
        vend = (Val *)(p->mem + p->mem_size);                                                      \
        vsp = vend + p->sp;                                                                        \
        vbase = vend - (base + 1);                                                                 \
    } while (0)

/* 稀疏发布：p->sp 只在 VM 边界（分配 / reserve / cfunc / builtin /
 * yield）前刷新，直线性片段里允许陈旧 —— 方向恒偏保守（发布栈深 ≥ 真实
 * 值，GC 多扫的只是死槽，只增保留不损正确性）。纪律：任何读 p->sp 的调
 * 用（proc_heap_alloc / proc_stack_reserve / proc_arena_grow /
 * do_builtin）之前必须 SP_PUB；SP_REFRESH_PTRS 位点因此总在发布之后。 */
#define SP_PUB() p->sp = -(int)(vend - vsp)

/* 撞堆兜底 = proc_push 的同款纪律（#248：帧入口 reserve 按 base+maxd
 * 覆盖整帧、堆侧 fit 以 floor 为界，正常永不命中）。arena 已持对象时不
 * 能 grow，撞穿即 fatal —— 与 proc_push 一致，不静默。 */
#define SP_ROOM()                                                                                  \
    do {                                                                                           \
        if ((char *)(vsp - 1) - (char *)p->mem < p->heap_ptr) {                                    \
            SP_PUB(); /* grow 的 memmove 按栈段搬移：p->sp 必须新鲜 */                  \
            if (proc_arena_grow(p, (int)((vend - vsp + 1) * (int)sizeof(Val))) != 0)               \
                ta_arena_fatal(p, "stack and heap meet before the new frame fits");                \
            SP_REFRESH_PTRS();                                                                     \
            if ((char *)(vsp - 1) - (char *)p->mem < p->heap_ptr)                                  \
                ta_arena_fatal(p, "stack and heap meet before the new frame fits");                \
        }                                                                                          \
    } while (0)
    long budget = 1000; /* 本片预算：对齐 scheduler.c MAX_REDUCTIONS */
    /* 采样 profiler（--profile）：采样边界 = 预算边界（见 NEXT()），关断时
     * 热循环零指令。状态不占 run_proc 局部量：开关直读 vm，时刻在 st。 */
    if (vm->prof_on)
        st->prof_last = prof_now_ns();
    /* 恢复：建帧（proc_frame_init）或阻塞唤醒（recv/ccall yield）发布的
     * LState 直接恢复，重执行阻塞指令。recv 不读 acc（恢复 nil）；ccall
     * yield 重入读 st->acc（CALL 语义末参在 acc，重执行时要重新 flush 进栈）。 */
    acc = st->has_acc ? st->acc : val_nil();
    st->has_acc = 0;
    base = st->base;
    cbase = W + st->cbase;
    pc = W + st->pc;
    cargs = st->cargs;
    /* 物化栈指针：slot i ↔ vend-(i+1)，vsp = vend-栈深，vbase =
     * vend-(base+1)。物化后把恢复点发布回 p —— do_builtin /
     * 边界协议都读 p->sp。 */
    vend = (Val *)(p->mem + p->mem_size);
    vsp = vend - st->sp;
    vbase = vend - (base + 1);
    p->sp = -(int)(vend - vsp);

    static void *dispatch[LOP_COUNT] = {
        [LOP_CONST] = &&op_const,     [LOP_LOAD] = &&op_load,
        [LOP_STORE] = &&op_store,     [LOP_LOADF] = &&op_loadf,
        [LOP_PUSH] = &&op_push,       [LOP_ADD] = &&op_add,
        [LOP_SUB] = &&op_sub,         [LOP_MUL] = &&op_mul,
        [LOP_DIV] = &&op_div,         [LOP_MOD] = &&op_mod,
        [LOP_LT] = &&op_lt,           [LOP_LE] = &&op_le,
        [LOP_GT] = &&op_gt,           [LOP_GE] = &&op_ge,
        [LOP_EQ] = &&op_eq,           [LOP_PAIRP] = &&op_pairp,
        [LOP_SYMP] = &&op_symp,       [LOP_CONS] = &&op_cons,
        [LOP_CAR] = &&op_car,         [LOP_CDR] = &&op_cdr,
        [LOP_JIF] = &&op_jif,         [LOP_JUMP] = &&op_jump,
        [LOP_CALL] = &&op_call,       [LOP_TCALL] = &&op_tcall,
        [LOP_RET] = &&op_ret,         [LOP_MAKE_CLOSURE] = &&op_make_closure,
        [LOP_GLOBAL] = &&op_global,   [LOP_RESERVE] = &&op_reserve,
        [LOP_BUILTIN] = &&op_builtin, [LOP_LOADP] = &&op_loadp,
        [LOP_CONSTP] = &&op_constp,   [LOP_GLOBP] = &&op_globp,
        [LOP_JIF_LT] = &&op_jif_lt,   [LOP_JIF_LE] = &&op_jif_le,
        [LOP_JIF_GT] = &&op_jif_gt,   [LOP_JIF_GE] = &&op_jif_ge,
        [LOP_JIF_EQ] = &&op_jif_eq,
    };

    goto *dispatch[*pc];

/* 轨迹：pc opcode sp/base acc */
#define TRACE                                                                                      \
    do {                                                                                           \
        if (vm->trace_left > 0) {                                                                  \
            vm->trace_left--;                                                                      \
            fprintf(stderr, "[%ld] op=%ld sp=%ld base=%ld acc=%016" PRIx64 "\n", (long)(pc - W),   \
                    *pc, (long)(vend - vsp), base, acc);                                           \
        }                                                                                          \
    } while (0)

/* 栈安全全部走编译期数据：bc 里每个 fn 带 maxd（depthpass 对 Ins 树算的
 * 帧内峰值 = fn 槽 + 实参 + 局部 + push 区）。检查只发生在 SP 变化位点，
 * 其它时候（含预算到界）一律不做（#248 重做）：
 *   - 帧入口（CALL/TCALL/RESERVE/first-run）按 base+maxd 一次
 *     proc_stack_reserve 预留整帧：对 floor 做一次 gap 检查（堆侧有对象
 *     就 gc、否则 grow），并记下 stack_floor（单调不回退）；
 *   - 帧内 PUSH/flush 走 proc_push，单次与堆顶比较；堆侧 fit 纪律保证
 *     heap <= lo - TA_HEAP_FIT_SLACK（lo = min(sp, floor)），契约内的
 *     push 永远留有余量，撞穿了是 ta_arena_fatal 响亮失败，不静默；
 *   - 堆侧分配（proc_heap_alloc / proc_reserve_heap / gc sizing）全部按
 *     proc_stack_lo 度量，heap 永不进入已预留帧区，monitor DOWN 深拷
 *     也走这条 fit。
 * 契约（帧内峰值 <= maxd）由编译期 depthpass 守。acc 可能持堆指针 ——
 * 收集的根集是整条栈，帧入口 flush、recv 根槽等在收集前已把该上栈的
 * 上了栈。
 *
 * 预算检查只挂在控制转移指令（TICK）：直线性片段摊薄到几十条指令才付
 * 一次 sub+branch（此前 NEXT 每指令一查，fib 实测 ~1.6x 差距）。判据
 * 每 1000 条才命中；单线程调度没有预算，一个持续可运行的 actor（比如
 * recv_after(0) 忙循环）会在一次 run_proc 里跑满安全网上限，饿死整个
 * runq —— timer-sleep-concurrency 的 busy actor 就是这么把 4 个 sleeper
 * 饿到超时的。预算耗尽 = 完整解释器态进 LState、返回 R_YIELD，入队交给
 * 调用者（worker_loop）—— 解释器入队即发布，发布后不得再触碰 st/p。 */
/* 预算检查只挂在控制转移指令（JIF/JUMP/CALL/TCALL/BUILTIN/RET 的 TICK）：
 * 直线性片段平均几十条指令才付一次 sub+branch，热循环里近乎免费。预算
 * 耗尽 = 完整解释器态进 LState、返回 R_YIELD（调用者重新入队）。采样边界
 * = 预算边界（关断时热循环零指令）。TICK 后 pc 已指向下一指令/转移目标——
 * 重执行语义不变。 */
#define TICK()                                                                                     \
    do {                                                                                           \
        if (--budget <= 0) {                                                                       \
            st->acc = acc;                                                                         \
            st->has_acc = 1;                                                                       \
            st->pc = (int)(pc - W);                                                                \
            st->base = base;                                                                       \
            st->cbase = (int)(cbase - W);                                                          \
            st->sp = (long)(vend - vsp);                                                           \
            st->cargs = cargs;                                                                     \
            if (vm->prof_on) {                                                                     \
                uint64_t now = prof_now_ns();                                                      \
                prof_collect(vm, p, now - st->prof_last);                                          \
                st->prof_last = now;                                                               \
            }                                                                                      \
            SP_PUB();                                                                              \
            return R_YIELD; /* state 仍 RUNNING：入队由 worker_loop 做 */                    \
        }                                                                                          \
    } while (0)

#define NEXT() goto *dispatch[*pc]

op_const:
    TRACE;
    acc = g_consts[pc[1]]; /* 全局常量表（#278）：不再驻留 proc 栈 */
    pc += 2;
    NEXT();
op_load:
    TRACE;
    acc = vbase[-(long)pc[1]];
    pc += 2;
    NEXT();
op_store:
    TRACE;
    vbase[-(long)pc[1]] = acc;
    pc += 2;
    NEXT();
op_loadf: {
    TRACE;
    Val clo = *vbase;
    if (val_tag(clo) != TAG_CLOS)
        fatal("LOADF on non-closure");
    HeapClosure *hc = (HeapClosure *)(uintptr_t)lpayload(clo);
    acc = hc->free[pc[1]];
    pc += 2;
    NEXT();
}
op_push:
    TRACE;
    SP_ROOM();
    *--vsp = acc;
    pc += 1;
    NEXT();

/* 融合指令（compile.ta fuse pass 发出）：x + PUSH 一步到位。acc 不动
 * ——与 LOAD/CONST/GLOB 的原语义一致（它们也是纯 acc 写）。 */
op_loadp:
    TRACE;
    SP_ROOM();
    *--vsp = vbase[-(long)pc[1]];
    pc += 2;
    NEXT();
op_constp:
    TRACE;
    SP_ROOM();
    *--vsp = g_consts[pc[1]];
    pc += 2;
    NEXT();
op_globp:
    TRACE;
    {
        long o = pc[1];
        SP_ROOM();
        *--vsp = o < 0 ? lbox(TAG_NATIVE, (uint64_t)(-(o + 1))) : lbox(TAG_CLOS_ID, (uint64_t)o);
    }
    pc += 2;
    NEXT();

/* RESERVE n：函数体首指令，把 sp 抬过整个局部变量区。
 *
 * 帧布局：base+0 = fn 指针，base+1..base+nargs = 实参，base+nargs+1..+4 =
 * cont 四槽，base+nargs+5 起是局部变量，求值栈（push 区 / 被调帧）从
 * base+nargs+5+nlocals 往上长。
 * 编译器负责发这条指令 —— 值栈是一根共享栈，VM 不该猜调用方要几个槽。
 * 预压 nil 而不是只抬 sp：这些 slot 必须落在 p->sp 区间内 GC 才看得见。 */
op_reserve:
    TRACE;
    /* 整帧空间一次预留：proc_push 的 grow 无 gc 兜底（heap 非空时 arena
     * 不能动），深递归逐 slot 撞车会直接 fatal。对齐 TA VM 的 CALL 纪律
     * （src/vm.c:1236 proc_stack_reserve(p, fp-4)）：能 grow 就 grow，
     * heap 已有对象就 gc_collect 腾位。lo 用 TA 负索引语义：tavm slot
     * i 对应 -(i+1)，预留后最低新 slot = sp+nlocals-1 → -(sp+nlocals)。 */
    SP_PUB(); /* reserve 的 fit/floor 读 p->sp */
    proc_stack_reserve(p, -(int)((long)(vend - vsp) + pc[1]));
    SP_REFRESH_PTRS(); /* reserve 可能 grow：mem/mem_size 已变 */
    for (long k = 0; k < pc[1]; k++) {
        *--vsp = val_nil(); /* reserve 已做整帧 gap 检查 */
    }
    pc += 2;
    NEXT();

/* 二元算术（LOP_ADD/SUB/MUL）：int∧int 保 int，其余
 * 走 double 路径（val_to_double 把非数值降为 0.0，与 golden 参考一致）。 */
#define ARITH(oper)                                                                                \
    do {                                                                                           \
        TRACE;                                                                                     \
        Val l = *vsp++;                                                                            \
        if (val_is_int(l) && val_is_int(acc)) {                                                    \
            int64_t a = val_get_int(l), b = val_get_int(acc);                                      \
            acc = val_int(a oper b);                                                               \
        } else {                                                                                   \
            acc = val_from_double(val_to_double(l) oper val_to_double(acc));                       \
        }                                                                                          \
        pc += 1;                                                                                   \
    } while (0)

op_add:
    ARITH(+);
    NEXT();
op_sub:
    ARITH(-);
    NEXT();
op_mul:
    ARITH(*);
    NEXT();
op_div: {
    TRACE;
    Val l = *vsp++;
    if (val_is_int(l) && val_is_int(acc)) {
        int64_t b = val_get_int(acc);
        if (b == 0)
            PROC_DIE_ISOLATED("divzero"); /* 整型除零：进程隔离，非停机 */
        acc = val_int(val_get_int(l) / b);
    } else {
        /* float/混合路径，除零给 ±inf 不 trap */
        acc = val_from_double(val_to_double(l) / val_to_double(acc));
    }
    pc += 1;
    NEXT();
}
op_mod: {
    TRACE;
    Val l = *vsp++;
    if (!val_is_int(l) || !val_is_int(acc))
        PROC_DIE_ISOLATED("arithtype"); /* 同 op_mod（issue #158） */
    int64_t b = val_get_int(acc);
    if (b == 0)
        PROC_DIE_ISOLATED("divzero");
    acc = val_int(val_get_int(l) % b);
    pc += 1;
    NEXT();
}

/* 比较指令（LOP_LT/LE/GT/GE）：acc = bool —— cmp_numeric_path 走
 * double，int∧int 走整数，其余（含 mixed 非数值）恒 false。 */
#define CMP(oper)                                                                                  \
    do {                                                                                           \
        TRACE;                                                                                     \
        Val l = *vsp++;                                                                            \
        int r;                                                                                     \
        if (cmp_numeric_path(l, acc)) {                                                            \
            r = val_to_double(l) oper val_to_double(acc);                                          \
        } else {                                                                                   \
            r = val_is_int(l) && val_is_int(acc) && (val_get_int(l) oper val_get_int(acc));        \
        }                                                                                          \
        acc = r ? val_true() : val_false();                                                        \
        pc += 1;                                                                                   \
    } while (0)

op_lt:
    CMP(<);
    NEXT();
op_le:
    CMP(<=);
    NEXT();
op_gt:
    CMP(>);
    NEXT();
op_ge:
    CMP(>=);
    NEXT();
op_eq:
    TRACE;
    {
        Val e = *vsp++;
        /* LOP_EQ：cmp_numeric_path 走 double（3 == 3.0），
         * 否则 val_equal —— 字符串按内容，immediate 按值，heap 按身份。 */
        int eq;
        if (cmp_numeric_path(e, acc))
            eq = val_to_double(e) == val_to_double(acc);
        else
            eq = val_equal(e, acc);
        acc = eq ? val_true() : val_false();
    }
    pc += 1;
    NEXT();

/* 融合比较+条件跳转（compile.ta fuse pass 对 if <cmp> 形态生成）：a = 栈顶
 * （左操作数），b = acc（右操作数），弹 a；真 → cbase+pc[1]，假 →
 * cbase+pc[2]（JIF 同款双目标回填）。语义 = IOp(cmp) + JIF 逐步执行的
 * 并；类型语义同 CMP（非数值对恒假）。 */
#define JIF_CMP(oper)                                                                              \
    do {                                                                                           \
        TRACE;                                                                                     \
        Val l = *vsp++;                                                                            \
        int r;                                                                                     \
        if (cmp_numeric_path(l, acc)) {                                                            \
            r = val_to_double(l) oper val_to_double(acc);                                          \
        } else {                                                                                   \
            r = val_is_int(l) && val_is_int(acc) && (val_get_int(l) oper val_get_int(acc));        \
        }                                                                                          \
        pc = r ? cbase + pc[1] : cbase + pc[2];                                                    \
    } while (0)

op_jif_lt:
    JIF_CMP(<);
    TICK();
    NEXT();
op_jif_le:
    JIF_CMP(<=);
    TICK();
    NEXT();
op_jif_gt:
    JIF_CMP(>);
    TICK();
    NEXT();
op_jif_ge:
    JIF_CMP(>=);
    TICK();
    NEXT();
op_jif_eq:
    TRACE;
    {
        Val e = *vsp++;
        /* 同 op_eq：cmp_numeric_path 走 double（3 == 3.0），否则 val_equal */
        int eq;
        if (cmp_numeric_path(e, acc))
            eq = val_to_double(e) == val_to_double(acc);
        else
            eq = val_equal(e, acc);
        pc = eq ? cbase + pc[1] : cbase + pc[2];
    }
    TICK();
    NEXT();

op_pairp:
    TRACE;
    acc = (val_tag(acc) == TAG_PAIR) ? val_true() : val_false();
    pc += 1;
    NEXT();
op_symp:
    TRACE;
    acc = (val_tag(acc) == TAG_SYM) ? val_true() : val_false();
    pc += 1;
    NEXT();

op_cons: {
    TRACE;
    /* 分配纪律：先 flush acc 上栈，
     * 分配后再从栈上读操作数 —— proc_heap_alloc 可能触发 moving GC，
     * 栈 slot 会被原地 forward，而 pop 进 C 局部的副本不会（悬垂指针
     * 进新 pair，下次 GC 读垃圾头："gc: unknown heap type 0"）。 */
    SP_ROOM();
    *--vsp = acc;
    SP_PUB(); /* 分配可能触发 GC：fit/根集读 p->sp */
    HeapPair *hp = (HeapPair *)proc_heap_alloc(p, sizeof(HeapPair));
    SP_REFRESH_PTRS(); /* 分配可能 grow：mem/mem_size 已变 */
    hp->hdr.type = HEAP_PAIR;
    hp->hdr.flags = 0;
    hp->cdr = vsp[0];
    hp->car = vsp[1];
    vsp += 2;
    acc = lbox(TAG_PAIR, (uint64_t)(uintptr_t)hp);
    pc += 1;
    NEXT();
}
op_car:
    TRACE;
    /* TA 语义（src/vm.c CASE_OP_CAR）：car(nil) = nil 优雅返回，只有
     * 非 pair 非 nil 才是运行期类型错误。此前对 nil 直接 fatal，driver 的
     * cdr(sig_types)（sig_types=nil 合法）一路撞死。 */
    if (val_tag(acc) == TAG_NIL) {
        acc = val_nil();
    } else if (val_tag(acc) != TAG_PAIR) {
        fprintf(stderr, "error: car: expected pair or nil, got tag=0x%04llx\n",
                (unsigned long long)val_tag(acc));
        PROC_DIE_ISOLATED("cartype");
    } else {
        acc = val_get_car(acc);
    }
    pc += 1;
    NEXT();
op_cdr:
    TRACE;
    /* 同 op_car：cdr(nil) = nil（TA 语义） */
    if (val_tag(acc) == TAG_NIL) {
        acc = val_nil();
    } else if (val_tag(acc) != TAG_PAIR) {
        fprintf(stderr, "error: cdr: expected pair or nil, got tag=0x%04llx\n",
                (unsigned long long)val_tag(acc));
        PROC_DIE_ISOLATED("cdrtype");
    } else {
        acc = val_get_cdr(acc);
    }
    pc += 1;
    NEXT();

op_jif:
    TRACE;
    pc = val_is_true(acc) ? cbase + pc[1] : cbase + pc[2];
    TICK();
    NEXT();
op_jump:
    TRACE;
    pc = cbase + pc[1];
    TICK();
    NEXT();

/* CALL/TCALL n（n = 参数数+1）：显式 continuation 协议（docs/backend.md）。
 * 概念上 call = 压 continuation + jump；CALL n 原子地完成
 *   flush acc（末参）→ 读 fn → native? do_ccall : arity 校验
 *   → 压 cont（ret_pc=pc+2, caller_base, caller_cbase, caller_cargs）四
 *   TAG_INT 槽 → reserve 整帧 → 切 base/pc/cbase。
 * 帧 = fn@base, args@base+1..n-1, cont@base+n+1..+4, locals 其上；RET 沿
 * cargs 寄存器定位 cont。cont 是栈上普通数据（TAG_INT 立即数，GC 惰性
 * 无须提前处理），返回栈 rstack 从此不存在。 */
/* ---- CALL/TCALL 真正公共的前缀 ----
 * 取参、flush acc（末参）、解 fn、arity 检查；完成后调用块 [fn, args..]
 * 在栈顶，fid/n 供 op_call / op_tcall 各自的后缀
 * （cont 落位 + 帧切换）使用。TAG_NATIVE 在此直接派发 do_ccall 不复返
 * ——两种尾向对 ccall 已无差别（结果落 fn 槽、pc+=2：op_call 落下一条
 * 指令，op_tcall 落编译器发的 trailing IRET，帧在 IRET 收口）。经
 * do-while 包不出作用域——后缀要消费前缀的局部量，故为裸序列，两个
 * op 各自带块作用域。 */
#define CALL_COMMON                                                                                \
    TRACE;                                                                                         \
    long n = pc[1];                                                                                \
    SP_ROOM(); /* flush acc（末参）：整帧已按 base+maxd 预留 */                         \
    *--vsp = acc;                                                                                  \
    Val fv = vsp[n - 1];                                                                           \
    uint64_t fid;                                                                                  \
    if (val_tag(fv) == TAG_CLOS_ID) {                                                              \
        fid = lpayload(fv);                                                                        \
    } else if (val_tag(fv) == TAG_CLOS) {                                                          \
        fid = (uint64_t)((HeapClosure *)(uintptr_t)lpayload(fv))->entry;                           \
    } else if (val_tag(fv) == TAG_NATIVE) {                                                        \
        /* 载荷 = link_unit 装载期解析好的 cfunc 下标。此时 cont 尚在帧内，     \
         * ccall 实参取栈顶 n-1 个，与帧簿记无涉。 */                                \
        SP_PUB(); /* do_ccall 按栈深 LSTK 工作；gc_enter 也按 p->sp 记 lo */              \
        if (do_ccall(vm, p, st, &acc, (long)lpayload(fv), n, base, (long)(pc - W),                 \
                     (long)(cbase - W), cargs, (long)(vend - vsp)))                                \
            return R_BLOCKED;                                                                      \
        SP_REFRESH_PTRS(); /* converge/reopen 可能 GC 换半区或 grow */                       \
        pc += 2;                                                                                   \
        NEXT();                                                                                    \
    } else {                                                                                       \
        fatal("call on non-function");                                                             \
    }                                                                                              \
    if (fn_args[fid] != n - 1)                                                                     \
        fatal("arity mismatch");

op_call: {
    CALL_COMMON;
    long nb = (long)(vend - vsp) - n; /* 调用块底 = 新帧 fn 槽深度 */
    /* 非尾：压 cont 四槽（纯 push，撞堆检查覆盖），fn/args 已是根。 */
    if ((char *)(vsp - 4) - (char *)p->mem < p->heap_ptr) {
        SP_PUB();
        if (proc_arena_grow(p, (int)((vend - vsp + 4) * (int)sizeof(Val))) != 0)
            ta_arena_fatal(p, "stack and heap meet before the new frame fits");
        SP_REFRESH_PTRS();
    }
    *--vsp = val_int((long)((pc + 2) - W)); /* ret_pc：CALL 双词，返回落到下一条 */
    *--vsp = val_int(base);                 /* caller_base */
    *--vsp = val_int((long)(cbase - W));    /* caller_cbase */
    *--vsp = val_int(cargs);                /* caller_cargs：RET 直读，省 fn 槽反查 */
    cargs = n - 1;
    /* cont 已入根集区间：reserve 可能 GC，cont 是立即数无 forwarding */
    SP_PUB();
    proc_stack_reserve(p, -(int)(nb + fn_maxd[fid] + 1));
    SP_REFRESH_PTRS();
    base = nb;
    vbase = vend - (base + 1);
    pc = W + fn_entry[fid];
    cbase = pc;
    TICK();
    NEXT();
}
op_tcall: {
    CALL_COMMON;
    /* 自尾调用 fast path：目标 == 当前帧 fn 槽的 fn（按 entry 判等，堆
     * closure 移动后 entry 不变）。同 fid 即同元数——CALL_COMMON 的
     * arity 检查给出 n-1 == fn_args[fid] == cargs——cont 新旧落位重合
     * （base+cargs+1..+4 == base+n..+3），cont 无须读写；同 fid + base
     * 不变即 fn_maxd 相同，帧已按 base+maxd 预留且 floor 单调不回退，
     * reserve 可整个跳过。只剩下拷 fn+实参、跳回 fn 入口。GC 安全：无
     * 分配位点，fn+args 在栈顶根集内，无 forwarding 危险。 */
    Val cfv = *vbase;
    uint64_t cfid =
        (val_tag(cfv) == TAG_CLOS_ID)
            ? lpayload(cfv)
            : (val_tag(cfv) == TAG_CLOS ? (uint64_t)((HeapClosure *)(uintptr_t)lpayload(cfv))->entry
                                        : (uint64_t)-1);
    if (cfid == fid) {
        for (long i = 0; i < n; i++)
            vbase[-(long)i] = vsp[n - 1 - i];
        vsp = vend - (base + n + 4);
        p->sp = -(int)(base + n + 4);
        pc = W + fn_entry[fid];
        cbase = pc;
        TICK();
        NEXT();
    }
    /* 尾调用：帧原地换人，base 不变。旧 cont（base+cargs+1..+4）是
     * TAG_INT 立即数，拷贝/回写无 forwarding 危险；必须先读 —— 新调用块
     * 更宽时会覆盖旧 cont 槽位。实参里的堆引用由 reserve 后的根集（栈顶
     * n 槽）保护；读升写升，源恒在目的上方，无自覆盖。 */
    long oret = (long)val_get_int(vbase[-(cargs + 1)]);
    long obase = (long)val_get_int(vbase[-(cargs + 2)]);
    long ocbase = (long)val_get_int(vbase[-(cargs + 3)]);
    long ocargs = (long)val_get_int(vbase[-(cargs + 4)]);
    SP_PUB(); /* reserve 的 fit/floor 读 p->sp */
    proc_stack_reserve(p, -(int)(base + fn_maxd[fid] + 1));
    SP_REFRESH_PTRS(); /* reserve 可能 grow */
    /* 新调用块（fn+n-1 参）下拷到 base；cont 写进新帧的 cont 位
     * （base+n..base+n+3 = RET 读的 base+cargs+1..+4，n=cargs+1）。
     * sp 恰到 cont 顶：深 base+n+4，顶槽 base+n+3，更陈旧的旧帧槽
     * 全部退出根集。 */
    for (long i = 0; i < n; i++)
        vbase[-(long)i] = vsp[n - 1 - i];
    vbase[-(long)n] = val_int(oret);
    vbase[-(long)(n + 1)] = val_int(obase);
    vbase[-(long)(n + 2)] = val_int(ocbase);
    vbase[-(long)(n + 3)] = val_int(ocargs);
    cargs = n - 1;
    vsp = vend - (base + n + 4);
    p->sp = -(int)(base + n + 4);
    pc = W + fn_entry[fid];
    cbase = pc;
    TICK();
    NEXT();
}

/* op_builtin：actor 原语。主体在 do_builtin（主循环外，别撑大派发热路径），
 * 这里只做取数、推进 pc、阻塞返回。 */
op_builtin:
    TRACE;
    SP_PUB(); /* do_builtin 内 proc_push/内置栈操作按 p->sp 写 */
    if (do_builtin(vm, p, st, &acc, pc[1], pc[2], (long)(pc - W), base, (long)(cbase - W), cargs,
                   (long)(vend - vsp)))
        return R_BLOCKED;
    SP_REFRESH_PTRS(); /* do_builtin 可能动栈（send 弹参 / arena grow）：重物化 */
    pc += 3;
    TICK();
    NEXT();

op_ret:
    TRACE;
    if (base == 0) {
        /* entry 返回 = 程序结束；子 proc 返回 = proc 终止（进退休表） */
        if (p->pid == vm->main_pid) {
            vm->exit_val = acc;
            atomic_store(&vm->entry_done, 1);
        } else {
            p->sp = -(int)(vend - vsp);
            proc_die(vm, p, val_nil());
        }
        return R_DIED;
    }
    /* 弹 cont（base+cargs+1..+4，cargs 寄存器定位）并恢复 caller 四
     * 寄存器：与 CALL 对称 —— CALL 压 cont+jump，RET 弹 cont+jump。
     * caller_cargs 就在 cont 第 4 槽，直读即得，无须 fn 槽反查。 */
    pc = W + (long)val_get_int(vbase[-(cargs + 1)]);
    cbase = W + (long)val_get_int(vbase[-(cargs + 3)]);
    vsp = vbase + 1; /* 弹整帧（fn+args+cont）：调用者栈深回到 base */
    p->sp = -(int)base;
    base = (long)val_get_int(vbase[-(cargs + 2)]); /* 读旧 vbase 后再切换 */
    /* RHS 求值仍用旧 cargs/旧 vbase；caller_cargs 在 cont 第 4 槽直读，
     * 无须 fn 槽反查。entry/子 proc 帧同为 CALL 压入的真值，无须特判。 */
    cargs = (int)(long)val_get_int(vbase[-(cargs + 4)]);
    vbase = vend - (base + 1);
    TICK();
    NEXT();

op_make_closure: {
    TRACE;
    long fnid = pc[1], nfree = pc[2];
    if (nfree == 0) {
        acc = lbox(TAG_CLOS_ID, (uint64_t)fnid);
    } else {
        SP_ROOM();
        *--vsp = acc; /* flush（最后一个自由值） */
        /* 自由值在 slot sp-nfree..sp-1，全在 p->sp 区间内。先分配（可能
         * 触发 GC，自由值因此被复制到新半区），再从栈上读——不能持指针。 */
        SP_PUB(); /* 分配可能触发 GC：fit/根集读 p->sp */
        HeapClosure *hc =
            (HeapClosure *)proc_heap_alloc(p, sizeof(HeapClosure) + (size_t)nfree * sizeof(Val));
        SP_REFRESH_PTRS(); /* 分配可能 grow：mem/mem_size 已变 */
        hc->hdr.type = HEAP_CLOS;
        hc->hdr.flags = 0;
        hc->entry = (int)fnid;
        hc->nfree = (int)nfree;
        for (long k = 0; k < nfree; k++)
            hc->free[k] = vsp[nfree - 1 - k];
        acc = lbox(TAG_CLOS, (uint64_t)(uintptr_t)hc);
        vsp += nfree;
    }
    pc += 3;
    NEXT();
}
op_global:
    TRACE;
    {
        long o = pc[1];
        /* 装载期负操作数 = link_unit 解析好的 cfunc 下标（-(cf+1)）；
         * 非负 = 闭包 fnid */
        acc = o < 0 ? lbox(TAG_NATIVE, (uint64_t)(-(o + 1))) : lbox(TAG_CLOS_ID, (uint64_t)o);
    }
    pc += 2;
    NEXT();
}
