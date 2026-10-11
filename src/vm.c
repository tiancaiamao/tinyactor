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

/* Equality for OP_EQ/OP_NE. Strings compare by content (HeapString holds
 * len + NUL-terminated data); ints/symbols/nil/true/false compare by their
 * NaN-boxed value (int payload is direct, symbols are interned); everything
 * else (pair/closure/bytes/pid) compares by pointer identity. */
int val_equal(Val a, Val b) {
    if (val_is_string(a) && val_is_string(b)) {
        HeapString *sa = val_get_string(a);
        HeapString *sb = val_get_string(b);
        return sa->len == sb->len && memcmp(sa->data, sb->data, sa->len) == 0;
    }
        return a == b;
}

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
    /* Same per-proc discipline as vm_yield (see above). OP_CCALL_NAME
     * checks die_requested when the C function returns and hands the
     * calling proc to proc_die — a builtin's equivalent of the opcode
     * type errors (cartype/cdrtype/divzero): die loudly near the cause
     * instead of returning a sentinel that propagates (issue #101). */
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
/* ================================================================
 * print_val
 * ================================================================ */
void print_val(VM *vm, Val v) {
    if (val_is_int(v)) {
        printf("%lld", (long long)val_get_int(v));
    } else if (val_is_float(v)) {
        /* %g: shortest decimal that round-trips; 3.14 → "3.14", 2.5 → "2.5",
         * 3.0 → "3", 1.0/0.0 → "inf". */
        printf("%g", val_get_float(v));
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
 * 解释器内部：报错出口、装载接缝、返回码、栈宏、内建派发
 * ================================================================ */

/* 进程级报错出口（ta.h 声明）：宿主与解释器共用一个，报完即退。 */
_Noreturn void fatal(const char *msg) {
    fprintf(stderr, "tavm: %s\n", msg);
    exit(1);
}

/* ---- 装载侧接缝：.tabc 装载器（main.c）装载期写入，run 期间只读 ---- */
extern long *W; /* 本单元的整数词流 */
extern long *fn_entry, *fn_args, *fn_maxd;
extern Val *g_consts; /* 全局常量表（#278，装载期 build_consts 建） */

#define R_DIED 0    /* proc 终止（entry RET = 程序结束；子 proc RET = 退休） */
#define R_BLOCKED 1 /* recv 阻塞：已登记 WAIT_RECV，等投递唤醒 */
/* 返回栈初始容量（word 数）。懒分配：不调用的 proc（如 worker 只 recv）
 * 零开销；曾预分配 192K words（1.5MB/proc），1M actor 空转出 1.5TB
 * VmSize + 4GB 首页 RSS。深递归由 CALL 处的翻倍 realloc 按需增长。 */
#define RSTACK_INIT_WORDS 4096

/* 运行期类型/算术错误的进程隔离 die（vm.c 同款：'divzero / 'arithtype /
 * 'cartype / 'cdrtype）：只死当前 proc，monitor/link 传播、其余进程继续
 * 全部走共享 proc_die。仅限 run_proc 内使用 —— 中途 die 要先发布 pc/sp
 * （GC 与根扫描要一致状态），再清理解释器栈返回 R_DIED。 */
#define PROC_DIE_ISOLATED(reason)                                                                  \
    do {                                                                                           \
        int die_sym = vm_intern_symbol(vm, reason);                                                \
        st->pc = pc;                                                                               \
        st->rsp = rsp; /* 栈行走要的现场发布在 proc_die 前（lisp_walk_stack 用）*/   \
        st->depth = depth;                                                                         \
        SP_SET(p, sp);                                                                             \
        proc_die(vm, p, val_symbol((uint32_t)die_sym));                                            \
        free(rstack);                                                                              \
        st->rstack = NULL;                                                                         \
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

/* ---- op_builtin 主体：actor 原语四条，放主循环外，别撑大派发热路径 ----
 *
 * 专用轻协议：前 n-1 参在栈顶（a1 = LSTK(p, sp-n+1) .. a(n-1) = LSTK(p, sp-1)），
 * 末参在 acc，无 fn 槽 —— 被调对象是编译期固化的整数 id（compile.ta B_*
 * 的镜像），switch 直接派发，零按名查找。结果写 *acc；tail 位置无需特殊
 * 处理（RET 会把 sp 收回 base）。零参/单参指令完全不动栈。
 * 返回非 0 = recv 阻塞：恢复点/栈已发布进 st，调用方直接 return R_BLOCKED。
 * LSTK 读 p 的栈：p 即本函数参数，LSTK/SP_* 宏显式带 p，无文件全局。 */
static int do_builtin(VM *vm, Proc *p, LState *st, Val *acc, long id, long n, long pc, long base,
                      long cbase, long sp, long depth, long rsp) {
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
        LState *nst = lstate_get(np->pid);
        nst->fnid = fid;
        nst->has_fn = 1;
        nst->fnval = owned; /* run_proc 首跑以临时栈根钉住它（见 run_proc） */
        runq_enqueue(vm, np->pid);
        *acc = val_pid((uint32_t)np->pid); /* 无栈参，sp 不动 */
        break;
    }
    case LB_SEND: { /* send (pid msg) -> nil：pid 在栈顶，msg 在 acc */
        if (n != 2)
            fatal("arity mismatch");
        uint32_t tpid = val_get_pid(LSTK(p, sp - 1));
        Proc *t = (tpid < (uint32_t)vm->procs_cap) ? vm->procs[tpid] : NULL;
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
            st->depth = depth;
            st->rsp = rsp;
            atomic_store(&p->state, PROC_WAIT_RECV);
            vm_wait_register(vm, p);
            pthread_mutex_unlock(&p->mbox_lock);
            return 1;
        }
        pthread_mutex_unlock(&p->mbox_lock);
        { /* tavm b_recv 同款：mbox_pop 的深拷贝关门分配会记 gc_pending，
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
            st->depth = depth;
            st->rsp = rsp;
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
            st->depth = depth;
            st->rsp = rsp;
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

int run_proc(VM *vm, Proc *p, LState *st) {
    int resume = st->started;
    tls_current_proc = p;
    long sp, depth, base, cbase, rsp, pc;
    long *rstack;
    Val acc;
    long budget = 1000; /* 本片预算：对齐 scheduler.c MAX_REDUCTIONS */
    /* 采样 profiler（--profile）：采样边界 = 预算边界（见 NEXT()），关断时
     * 热循环零指令。状态不占 run_proc 局部量：开关直读 vm，时刻在 st。 */
    if (vm->prof_on)
        st->prof_last = prof_now_ns();
    if (!resume) {
        /* 返回栈懒分配：首跑不 malloc，首次 CALL 压帧时经宏内翻倍 realloc
         * 从 NULL 起步（realloc(NULL,n)==malloc）。只 recv 不调用的 actor
         * （1M worker 场景）从此零 rstack 开销 —— 预分配曾让 1M proc 空转
         * 出 1.5TB VmSize。 */
        rstack = NULL;
        if (st->has_fn) {
            /* 子 proc（#278）：首跑只物化帧头（fn 槽 + 实参 nil），与
             * CALL 后的 callee 入口同形 —— body 从 depthpass 的
             * d0 = 1+nargs 起长，RESERVE 抬局部、求值 push 各长一次，
             * 峰值 ≤ maxd。若预压整帧（1+maxd），body 会在其上长
             * 第二次、越过 floor（#248 撤每指令边界后必然撞 heap）。
             * fnval 此刻只被 C 变量持有，reserve 可能 GC（根集 = 栈）——
             * 先压进 fn 槽（slot 0，base = 0）发布成根，reserve 后再补
             * 实参 nil（不再分配）。 */
            long total = 1 + fn_maxd[st->fnid];
            long npre = 1 + fn_args[st->fnid];
            proc_push(p, st->fnval);
            sp = 1;
            SP_SET(p, sp);
            proc_stack_reserve(p, -(int)total);
            for (long k = 1; k < npre; k++)
                proc_push(p, val_nil());
            sp = npre;
            SP_SET(p, sp);
        } else {
            /* entry（#278）：同形 —— 物化帧头 1+nargs 个 nil，floor 记
             * 1+maxd 覆盖 body 的 RESERVE + 求值峰值（此前 entry 没有
             * reserve，floor 停在 0，求值 push 全在契约外）。 */
            long npre = 1 + fn_args[st->fnid];
            sp = 0;
            for (long k = 0; k < npre; k++)
                proc_push(p, val_nil());
            sp = npre;
            proc_stack_reserve(p, -(int)(1 + fn_maxd[st->fnid]));
        }
        base = 0;
        pc = fn_entry[st->fnid];
        cbase = pc; /* JIF/JUMP 目标 = fn 内相对偏移 + cbase */
        depth = rsp = 0;
        acc = val_nil();
        st->started = 1;
    } else {
        /* 阻塞唤醒：LState 保存的完整解释器态直接恢复，重执行阻塞指令。
         * recv 不读 acc（恢复 nil）；ccall yield 重入读 st->acc（CALL
         * 语义末参在 acc，重执行时要重新 flush 进栈）。 */
        acc = st->has_acc ? st->acc : val_nil();
        st->has_acc = 0;
        sp = st->sp;
        base = st->base;
        cbase = st->cbase;
        pc = st->pc;
        depth = st->depth;
        rsp = st->rsp;
        rstack = st->rstack;
    }
    SP_SET(p, sp);

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
    };

    goto *dispatch[W[pc]];

/* 轨迹：pc opcode sp/base acc */
#define TRACE                                                                                      \
    do {                                                                                           \
        if (vm->trace_left > 0) {                                                                  \
            vm->trace_left--;                                                                      \
            fprintf(stderr, "[%ld] op=%ld sp=%ld base=%ld acc=%016" PRIx64 "\n", pc, W[pc], sp,    \
                    base, acc);                                                                    \
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
 * 预算检查保持每指令一条 sub+branch（到界判据本身无法批量去掉）：判据
 * 每 1000 条才命中一次，分支预测近乎免费；单线程调度没有预算，一个持续
 * 可运行的 actor（比如 recv_after(0) 忙循环）会在一次 run_proc 里跑满
 * 安全网上限，饿死整个 runq —— timer-sleep-concurrency 的 busy actor
 * 就是这么把 4 个 sleeper 饿到超时的。预算耗尽 = 完整解释器态进 LState、
 * 重新入队、回 sched，期间不做任何余量工作。 */
#define NEXT()                                                                                                        \
    do {                                                                                                              \
        if (--budget <= 0) {                                                                                          \
            st->acc = acc;                                                                                            \
            st->has_acc = 1;                                                                                          \
            st->pc = pc;                                                                                              \
            st->base = base;                                                                                          \
            st->cbase = cbase;                                                                                        \
            st->sp = sp;                                                                                              \
            st->depth = depth;                                                                                        \
            st->rsp = rsp;                                                                                            \
            if (vm->prof_on) {                                                                                        \
                /* 采样边界 = 预算边界（每 1000 条指令一次）：关断时这条在 1/1000 的          \
                 * 冷分支里再判一次，热路径不为采样多付一条指令（旧的每指令 r++ 曾实测 \
                 * +5%）。叶帧：上面的 state 保存已把 st->pc 发到采样时刻。 */                     \
                uint64_t now = prof_now_ns();                                                                         \
                prof_collect(vm, p, now - st->prof_last);                                                             \
                st->prof_last = now;                                                                                  \
            }                                                                                                         \
            SP_SET(p, sp);                                                                                            \
            runq_enqueue(vm, p->pid); /* state 仍 RUNNING，sched 直接重跑 */                                    \
            return R_BLOCKED;                                                                                         \
        }                                                                                                             \
        goto *dispatch[W[pc]];                                                                                        \
    } while (0)

op_const:
    TRACE;
    acc = g_consts[W[pc + 1]]; /* 全局常量表（#278）：不再驻留 proc 栈 */
    pc += 2;
    NEXT();
op_load:
    TRACE;
    acc = LSTK(p, base + W[pc + 1]);
    pc += 2;
    NEXT();
op_store:
    TRACE;
    LSTK(p, base + W[pc + 1]) = acc;
    pc += 2;
    NEXT();
op_loadf: {
    TRACE;
    Val clo = LSTK(p, base);
    if (val_tag(clo) != TAG_CLOS)
        fatal("LOADF on non-closure");
    HeapClosure *hc = (HeapClosure *)(uintptr_t)lpayload(clo);
    acc = hc->free[W[pc + 1]];
    pc += 2;
    NEXT();
}
op_push:
    TRACE;
    proc_push(p, acc);
    SP_ADJ(p, 1);
    pc += 1;
    NEXT();

/* 融合指令（compile.ta fuse pass 发出）：x + PUSH 一步到位。acc 不动
 * ——与 LOAD/CONST/GLOB 的原语义一致（它们也是纯 acc 写）。 */
op_loadp:
    TRACE;
    proc_push(p, LSTK(p, base + W[pc + 1]));
    SP_ADJ(p, 1);
    pc += 2;
    NEXT();
op_constp:
    TRACE;
    proc_push(p, g_consts[W[pc + 1]]);
    SP_ADJ(p, 1);
    pc += 2;
    NEXT();
op_globp:
    TRACE;
    {
        long o = W[pc + 1];
        proc_push(p,
                  o < 0 ? lbox(TAG_NATIVE, (uint64_t)(-(o + 1))) : lbox(TAG_CLOS_ID, (uint64_t)o));
    }
    SP_ADJ(p, 1);
    pc += 2;
    NEXT();

/* RESERVE n：函数体首指令，把 sp 抬过整个局部变量区。
 *
 * 帧布局：base+0 = fn 指针，base+1..base+nargs = 实参，base+nargs+1 起是
 * 局部变量，求值栈（push 区 / 被调帧）从 base+nargs+1+nlocals 往上长。
 * 编译器负责发这条指令 —— 值栈是一根共享栈，VM 不该猜调用方要几个槽。
 * 预压 nil 而不是只抬 sp：这些 slot 必须落在 p->sp 区间内 GC 才看得见。 */
op_reserve:
    TRACE;
    /* 整帧空间一次预留：proc_push 的 grow 无 gc 兜底（heap 非空时 arena
     * 不能动），深递归逐 slot 撞车会直接 fatal。对齐 TA VM 的 CALL 纪律
     * （src/vm.c:1236 proc_stack_reserve(p, fp-4)）：能 grow 就 grow，
     * heap 已有对象就 gc_collect 腾位。lo 用 TA 负索引语义：tavm slot
     * i 对应 -(i+1)，预留后最低新 slot = sp+nlocals-1 → -(sp+nlocals)。 */
    proc_stack_reserve(p, -(int)(sp + W[pc + 1]));
    for (long k = 0; k < W[pc + 1]; k++)
        proc_push(p, val_nil());
    SP_ADJ(p, W[pc + 1]);
    pc += 2;
    NEXT();

/* 二元算术：镜像 src/vm.c OP_ADD/OP_SUB/OP_MUL —— int∧int 保 int，其余
 * 走 double 路径（val_to_double 把非数值降为 0.0，与 golden 参考一致）。 */
#define ARITH(oper)                                                                                \
    do {                                                                                           \
        TRACE;                                                                                     \
        Val l = proc_pop(p);                                                                       \
        SP_ADJ(p, -1);                                                                             \
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
    Val l = proc_pop(p);
    SP_ADJ(p, -1);
    if (val_is_int(l) && val_is_int(acc)) {
        int64_t b = val_get_int(acc);
        if (b == 0)
            PROC_DIE_ISOLATED("divzero"); /* 同 vm.c OP_DIV：进程隔离，非停机 */
        acc = val_int(val_get_int(l) / b);
    } else {
        /* 镜像 vm.c OP_DIV：float/混合路径，除零给 ±inf 不 trap */
        acc = val_from_double(val_to_double(l) / val_to_double(acc));
    }
    pc += 1;
    NEXT();
}
op_mod: {
    TRACE;
    Val l = proc_pop(p);
    SP_ADJ(p, -1);
    if (!val_is_int(l) || !val_is_int(acc))
        PROC_DIE_ISOLATED("arithtype"); /* 同 vm.c CASE_OP_MOD（issue #158） */
    int64_t b = val_get_int(acc);
    if (b == 0)
        PROC_DIE_ISOLATED("divzero");
    acc = val_int(val_get_int(l) % b);
    pc += 1;
    NEXT();
}

/* 比较：acc = bool。镜像 src/vm.c OP_LT/LE/GT/GE —— cmp_numeric_path 走
 * double，int∧int 走整数，其余（含 mixed 非数值）恒 false。 */
#define CMP(oper)                                                                                  \
    do {                                                                                           \
        TRACE;                                                                                     \
        Val l = proc_pop(p);                                                                       \
        SP_ADJ(p, -1);                                                                             \
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
        Val e = proc_pop(p);
        SP_ADJ(p, -1);
        /* 镜像 vm.c OP_EQ：cmp_numeric_path 走 double（3 == 3.0），
         * 否则 val_equal —— 字符串按内容，immediate 按值，heap 按身份。
         * 两者都是 src/vm.o 的导出，单一实现不复制。 */
        int eq;
        if (cmp_numeric_path(e, acc))
            eq = val_to_double(e) == val_to_double(acc);
        else
            eq = val_equal(e, acc);
        acc = eq ? val_true() : val_false();
    }
    pc += 1;
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
    /* 对齐 TA VM 的 OP_CONS 纪律（src/vm.c:790）：先 flush acc 上栈，
     * 分配后再从栈上读操作数 —— proc_heap_alloc 可能触发 moving GC，
     * 栈 slot 会被原地 forward，而 pop 进 C 局部的副本不会（悬垂指针
     * 进新 pair，下次 GC 读垃圾头："gc: unknown heap type 0"）。 */
    proc_push(p, acc);
    SP_ADJ(p, 1);
    HeapPair *hp = (HeapPair *)proc_heap_alloc(p, sizeof(HeapPair));
    hp->hdr.type = HEAP_PAIR;
    hp->hdr.flags = 0;
    hp->cdr = LSTK(p, sp - 1);
    hp->car = LSTK(p, sp - 2);
    SP_ADJ(p, -2);
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
    pc = val_is_true(acc) ? cbase + W[pc + 1] : cbase + W[pc + 2];
    NEXT();
op_jump:
    TRACE;
    pc = cbase + W[pc + 1];
    NEXT();

/* CALL/TCALL n（n = 参数数+1）：flush acc（末参）→ base = sp-n。
 * 栈容量由 proc_push 自动增长兜底，不再检查 stack_cap。 */
#define CALL_COMMON(is_tail)                                                                                            \
    do {                                                                                                                \
        TRACE;                                                                                                          \
        long n = W[pc + 1];                                                                                             \
        proc_push(p, acc);                                                                                              \
        SP_ADJ(p, 1);                                                                                                   \
        long nb = sp - n;                                                                                               \
        Val fv = LSTK(p, nb);                                                                                           \
        uint64_t fid;                                                                                                   \
        if (val_tag(fv) == TAG_CLOS_ID) {                                                                               \
            fid = lpayload(fv);                                                                                         \
        } else if (val_tag(fv) == TAG_CLOS) {                                                                           \
            fid = (uint64_t)((HeapClosure *)(uintptr_t)lpayload(fv))->entry;                                            \
        } else if (val_tag(fv) == TAG_NATIVE) {                                                                         \
            /* 载荷 = link_unit 装载期解析好的 cfunc 下标：直接调用，零按名查找 */                \
            long cf = (long)lpayload(fv);                                                                               \
            if (cf >= vm->cfunc_count)                                                                                  \
                fatal("bad cfunc index");                                                                               \
            /* nargs==-1=声明的变参（net.connect 可选 timeout），个数由 cfunc 自校验；宿主 VM        \
             * 运行期不查 arity，此处只对固定参保留保险 */                                             \
            if (vm->cfuncs[cf].nargs >= 0 && vm->cfuncs[cf].nargs != (int)(n - 1))                                      \
                fatal("arity mismatch");                                                                                \
            /* 实参按下标逐个拷进 C 数组 —— 不能取 &LSTK(p, nb+1) 当基址：                        \
             * 栈向低地址增长，C 数组方向（地址递增）与 slot 序（索引递增、                   \
             * 地址递减）相反，args[1] 会读到 fn 槽（bug：str.concat 恒空串）。*/                    \
            Val cargs[64];                                                                                              \
            if (n - 1 > 64)                                                                                             \
                fatal("too many arguments");                                                                            \
            for (int ai = 0; ai < (int)(n - 1); ai++)                                                                   \
                cargs[ai] = LSTK(p, nb + 1 + ai);                                                                       \
            /* 照抄 TA VM 的 OP_CCALL_NAME 协议（src/vm.c:1570-1622）：关门 +                                 \
             * in_ccall 窗口 —— 回调经 proc_heap_alloc 的分配改走 chunk arena，                          \
             * 不触发 moving GC，C 局部里的 Val 全程有效；返回后 converge 把                           \
             * chunk 结果收敛进 heap，否则结果 Val 在下次 GC 后悬垂。*/                                \
            proc_gc_enter(p);                                                                                           \
            p->in_ccall = 1;                                                                                            \
            p->yield_requested = 0;                                                                                     \
            Val r = vm->cfuncs[cf].fn(vm, cargs, (int)(n - 1));                                                         \
            if (p->yield_requested) {                                                                                   \
                /* 三段非阻塞 cfunc（net.connect/read…）请求重入：宿主协议同款                      \
                 * （src/vm.c OP_CCALL_NAME）—— pc 回到本指令起点，撤回入口处                        \
                 * flush 的 acc（重执行时重压），完整解释器态进 LState，WAIT_IO                       \
                 * 登记，io poller 唤醒后重执行本条 CALL。 */                                               \
                p->yield_requested = 0;                                                                                 \
                p->in_ccall = 0;                                                                                        \
                proc_chunk_reset(p);                                                                                    \
                proc_gc_leave(p);                                                                                       \
                st->acc = acc;                                                                                          \
                st->has_acc = 1;                                                                                        \
                st->pc = pc;                                                                                            \
                st->base = base;                                                                                        \
                st->cbase = cbase;                                                                                      \
                st->depth = depth;                                                                                      \
                st->rsp = rsp;                                                                                          \
                SP_SET(p, nb + n - 1); /* 撤回入口 flush 的 acc：回到指令入口 sp，重执行时重压 */    \
                st->sp = sp;                                                                                            \
                atomic_store(&p->state, PROC_WAIT_IO);                                                                  \
                vm_wait_register(vm, p);                                                                                \
                return R_BLOCKED;                                                                                       \
            }                                                                                                           \
            p->in_ccall = 0;                                                                                            \
            proc_chunk_converge(p, &r);                                                                                 \
            SP_SET(p, is_tail ? base : nb); /* 实参已消费，先弹掉（gate 仍关，无收集风险）*/         \
            proc_push(p, r); /* 结果入栈作为根：下面的 drain 收集只扫栈 */                              \
            SP_ADJ(p, 1);                                                                                               \
            proc_gc_reopen(p); /* tavm OP_CCALL 同款：补偿被关门压下的 GC 请求，                          \
                                * 否则纯 ccall 工作负载（json/str 解析）请求永不清、堆只增不收 */ \
            acc = LSTK(p, sp - 1); /* 收集可能搬移对象，从栈重读（不能直接用 r） */                 \
            SP_SET(p, is_tail ? base : nb); /* 弹掉临时的结果根槽 */                                           \
            pc += 2;                                                                                                    \
            NEXT();                                                                                                     \
        } else {                                                                                                        \
            fatal("call on non-function");                                                                              \
        }                                                                                                               \
        if (fn_args[fid] != n - 1)                                                                                      \
            fatal("arity mismatch");                                                                                    \
        /* callee 整帧一次预留（base+maxd，maxd 含 fn 槽+实参+局部+push 峰值）：                      \
         * SP 变化指令处的唯一栈检查。此后 callee 内 PUSH/RESERVE 全落在                              \
         * 预留区间，无需任何边界检查。reserve 可能 gc（堆对象移动），                          \
         * fid 已提取成 long 不受影响；acc 已 flush 在栈上，GC 原地 forward。 */                       \
        if (is_tail) {                                                                                                  \
            proc_stack_reserve(p, -(int)(base + fn_maxd[fid] + 1));                                                     \
            /* 帧恰 n 个 slot（fn + n-1 参）：多拷/多发布一个 slot 会把陈旧                           \
             * 栈垃圾放进 GC 根集扫描范围 —— GC 把陈旧堆指针当根，从                           \
             * fromspace 已回收区读垃圾头（"gc: unknown heap type 0"）。*/                                   \
            for (long i = 0; i < n; i++)                                                                                \
                LSTK(p, base + i) = LSTK(p, nb + i);                                                                    \
            nb = base;                                                                                                  \
            SP_SET(p, nb + n);                                                                                          \
        } else {                                                                                                        \
            proc_stack_reserve(p, -(int)(nb + fn_maxd[fid] + 1));                                                       \
            if (rsp + 3 > st->rstack_cap) {                                                                             \
                /* 返回栈满：翻倍 realloc（未分配时起步 RSTACK_INIT_WORDS，\ */                          \
                /* realloc(NULL,n)==malloc，支撑懒分配）。指针经 st->rstack \ */                             \
                /* 持久化，阻塞重入的 resume 路径会重读，旧副本不会复活。 */                     \
                long rcap = st->rstack_cap ? st->rstack_cap * 2 : RSTACK_INIT_WORDS;                                    \
                long *nrs = realloc(st->rstack, (size_t)rcap * sizeof(long));                                           \
                if (!nrs)                                                                                               \
                    fatal("out of memory");                                                                             \
                rstack = nrs;                                                                                           \
                st->rstack = nrs;                                                                                       \
                st->rstack_cap = rcap;                                                                                  \
            }                                                                                                           \
            rstack[rsp++] = pc + 2;                                                                                     \
            rstack[rsp++] = base;                                                                                       \
            rstack[rsp++] = cbase;                                                                                      \
            depth++;                                                                                                    \
        }                                                                                                               \
        pc = fn_entry[fid];                                                                                             \
        base = nb;                                                                                                      \
        cbase = pc;                                                                                                     \
    } while (0)

op_call:
    CALL_COMMON(0);
    NEXT();
op_tcall:
    CALL_COMMON(1);
    NEXT();

/* op_builtin：actor 原语。主体在 do_builtin（主循环外，别撑大派发热路径），
 * 这里只做取数、推进 pc、阻塞返回。 */
op_builtin:
    TRACE;
    if (do_builtin(vm, p, st, &acc, W[pc + 1], W[pc + 2], pc, base, cbase, sp, depth, rsp))
        return R_BLOCKED;
    sp = -p->sp; /* do_builtin 可能动栈（send 弹参），局部 sp 从权威源恢复 */
    pc += 3;
    NEXT();

op_ret:
    TRACE;
    if (depth == 0) {
        /* entry 返回 = 程序结束；子 proc 返回 = proc 终止（进退休表） */
        if (p->pid == vm->main_pid) {
            vm->exit_val = acc;
            vm->entry_done = 1;
        } else {
            SP_SET(p, sp);
            proc_die(vm, p, val_nil());
        }
        free(rstack);
        st->rstack = NULL;
        return R_DIED;
    }
    depth--;
    SP_SET(p, base);
    cbase = rstack[--rsp];
    base = rstack[--rsp];
    pc = rstack[--rsp];
    NEXT();

op_make_closure: {
    TRACE;
    long fnid = W[pc + 1], nfree = W[pc + 2];
    if (nfree == 0) {
        acc = lbox(TAG_CLOS_ID, (uint64_t)fnid);
    } else {
        proc_push(p, acc); /* flush（最后一个自由值） */
        SP_ADJ(p, 1);
        /* 自由值在 slot sp-nfree..sp-1，全在 p->sp 区间内。先分配（可能
         * 触发 GC，自由值因此被复制到新半区），再从栈上读——不能持指针。 */
        HeapClosure *hc =
            (HeapClosure *)proc_heap_alloc(p, sizeof(HeapClosure) + (size_t)nfree * sizeof(Val));
        hc->hdr.type = HEAP_CLOS;
        hc->hdr.flags = 0;
        hc->entry = (int)fnid;
        hc->nfree = (int)nfree;
        for (long k = 0; k < nfree; k++)
            hc->free[k] = LSTK(p, sp - nfree + k);
        acc = lbox(TAG_CLOS, (uint64_t)(uintptr_t)hc);
        SP_ADJ(p, -nfree);
    }
    pc += 3;
    NEXT();
}
op_global:
    TRACE;
    {
        long o = W[pc + 1];
        /* 装载期负操作数 = link_unit 解析好的 cfunc 下标（-(cf+1)）；
         * 非负 = 闭包 fnid */
        acc = o < 0 ? lbox(TAG_NATIVE, (uint64_t)(-(o + 1))) : lbox(TAG_CLOS_ID, (uint64_t)o);
    }
    pc += 2;
    NEXT();
}
