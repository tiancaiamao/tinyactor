/* rt.c — C 后端（cgen.ta 生成代码）的运行时宿主。
 *
 * 这个 TU 与字节码后端的 src/vm.c 是**同位替代**：链接时二选一
 * （tac = 运行时对象 - vm.o - main.o + rt.o + 生成代码），所以这里补上
 * vm.c 唯一对外可见的几个符号（run_proc / fatal / tls_current_proc /
 * vm_fn_name / vm_walk_stack），其余全部复用 TA 本体：堆与 GC（gc.c）、
 * 调度（scheduler.c）、值表示与分配（val.c）、C 模块（api/builtin）、
 * 宿主初始化（host.c）。GC 的根集契约原样继承：扫 Proc 栈 [p->sp, 0)。
 *
 * 生成代码的契约见 src/rt.h 头部。
 */
#include "rt.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

/* vm.c 的同名定义（排除 vm.o 后由这里补）：C 模块（cfunc）靠它找到
 * "当前 proc"。线程局部 —— 每个 worker 线程只跑一个 VM 的 proc。 */
__thread Proc *tls_current_proc;

/* stage 里怎么拿到 VM：与 tls_current_proc 同款的线程局部，不引入跨线程
 * 可变全局（vm.c 之外没有第二条通路能拿到 VM，proc 也不回指 VM）。 */
static __thread VM *rt_tls_vm;

VM *rt_vm(void) { return rt_tls_vm; }

void fatal(const char *msg) {
    fprintf(stderr, "tac: %s\n", msg);
    exit(1);
}

/* 栈行走/帧名：字节码后端的 walker 绑 .tabc 的 frame 图，C 后端的帧链是
 * C 栈（原生栈溢出由 OS 报），M1 暂不提供回溯名称 —— 返回 0/NULL 让
 * prof.c 与崩溃上报走"无深度"路径（scheduler.c 判 main_crashed 不受影响）。 */
int vm_walk_stack(const VM *vm, const Proc *p, int *out, int max_depth) {
    (void)vm, (void)p, (void)out, (void)max_depth;
    return 0;
}

const char *vm_fn_name(const VM *vm, int fid) {
    (void)vm, (void)fid;
    return NULL;
}

/* ---- 进程隔离死亡 ----
 *
 * 与 vm.c 的 PROC_DIE_ISOLATED 同语义：只死当前 proc（monitor/link 传播，
 * 其余进程继续）。差别只在"怎么离开"：字节码是解释循环，return R_DIED 就
 * 够了；C 后端的调用链是一条 musttail 链，退到 run_proc 要跨过任意深度的
 * C 调用（算术/car 的 inline 判定都在链上），所以用 longjmp 回到 run_proc
 * 的 setjmp 点 —— 现场（pc/base/sp）先发布，proc_die 已把 proc 摘掉。 */
static __thread jmp_buf rt_die_jb;
static __thread int rt_die_ready;

_Noreturn void rt_die(Proc *p, const char *reason) {
    VM *vm = rt_tls_vm;
    int sym = (int)vm_intern_symbol(vm, reason);
    proc_die(vm, p, val_symbol((uint32_t)sym));
    if (rt_die_ready)
        longjmp(rt_die_jb, 1);
    fatal("proc died outside run_proc");
}

/* ---- 分配 ----
 *
 * 纪律同 vm.c 的 op_cons：操作数留在槽里（生成代码已 spill 并发布 sp），
 * 分配后再从槽读 —— proc_heap_alloc 可能 moving GC，槽会被原地 forward，
 * 而 C 局部副本不会（悬垂指针进新 pair，下一轮 GC 读垃圾头）。 */

Val rt_cons_at(Proc *p, int a_slot, int b_slot) {
    HeapPair *hp = (HeapPair *)proc_heap_alloc(p, sizeof(HeapPair));
    hp->hdr.type = HEAP_PAIR;
    hp->hdr.flags = 0;
    hp->car = RT_SLOT(p, a_slot);
    hp->cdr = RT_SLOT(p, b_slot);
    return lbox(TAG_PAIR, (uint64_t)(uintptr_t)hp);
}

/* 闭包：nfree==0 退化成 TAG_CLOS_ID（零堆分配的全局函数引用，与
 * vm.c 的 op_make_closure 同款）。自由值从槽 [at, at+nfree) 读。 */
Val rt_mkclos(Proc *p, int stage, int nfree, int at_slot) {
    if (nfree == 0)
        return lbox(TAG_CLOS_ID, (uint64_t)stage);
    HeapClosure *hc =
        (HeapClosure *)proc_heap_alloc(p, sizeof(HeapClosure) + (size_t)nfree * sizeof(Val));
    hc->hdr.type = HEAP_CLOS;
    hc->hdr.flags = 0;
    hc->entry = stage;
    hc->nfree = nfree;
    for (int k = 0; k < nfree; k++)
        hc->free[k] = RT_SLOT(p, at_slot + k);
    return lbox(TAG_CLOS, (uint64_t)(uintptr_t)hc);
}

/* ---- 常量 ----
 *
 * 字符串常量 malloc 直建在 proc 堆外（与 main.c 的 build_consts 同款）：
 * TAG_STRING 的 payload 不在 arena 里，gc_copy_val 的 in_fromspace 判定
 * 直接跳过 —— 所以它能安全地驻留在生成代码的 C 全局里，永不搬移。TA
 * 字符串不可变（str.* 全走读访问器），全局共享一份无写者风险。 */
Val rt_const_string(VM *vm, const char *s, int len) {
    (void)vm;
    HeapString *hs = (HeapString *)malloc(sizeof(HeapString) + (size_t)len + 1);
    if (!hs)
        fatal("oom");
    hs->hdr.type = HEAP_STRING;
    hs->hdr.flags = 0;
    hs->len = len;
    memcpy(hs->data, s, (size_t)len);
    hs->data[len] = '\0';
    return lbox(TAG_STRING, (uint64_t)(uintptr_t)hs);
}

/* 符号常量：进程级 intern（同名同 id，eq? 按位比较才成立）。 */
Val rt_const_symbol(VM *vm, const char *s) {
    return val_symbol((uint32_t)vm_intern_symbol(vm, s));
}

/* 全局名 → cfunc 下标（GLOBAL 按名解析，与 main.c link_unit 同语义：
 * 解析不到就是生成期/装载期的错误，不静默压 nil）。 */
int rt_cfunc(VM *vm, const char *name) {
    int cf = vm_find_cfunc(vm, name);
    if (cf < 0) {
        char msg[256];
        snprintf(msg, sizeof msg, "unknown C function: %s", name);
        fatal(msg);
    }
    return cf;
}


/* ---- C 模块调用 ----
 *
 * 窗口协议与 vm.c 的 do_ccall 逐条对齐（arena 关门 → 调用 → 收敛 → 重开），
 * 因为 cfunc 里分配走的是同一条堆路径。实参先拷进 C 数组：关门期间不收集，
 * 所以数组里的副本在 cfunc 存活期内不会被搬走。
 *
 * 未支持：cfunc 主动 yield（net/timer 的非阻塞挂起）。字节码后端的恢复点
 * 是单条指令，重执行只有 cfunc 自身；C 后端的恢复点是一个 stage，重跑会
 * 重做它前面的副作用。M2 给每个 ccall 调用点生成 retry stage 后再接。 */
Val rt_ccall_at(Proc *p, int cf, int at_slot, int nargs) {
    VM *vm = rt_tls_vm;
    if (cf < 0 || cf >= vm->cfunc_count || !vm->cfuncs[cf].fn)
        fatal("bad cfunc index");
    if (nargs > 64)
        fatal("too many arguments");
    Val cargv[64];
    for (int i = 0; i < nargs; i++)
        cargv[i] = RT_SLOT(p, at_slot + i);

    proc_gc_enter(p);
    CallbackArena arena;
    callback_arena_init(&arena);
    p->ccall = &arena;
    p->yield_requested = 0;
    Val r = vm->cfuncs[cf].fn(vm, cargv, nargs);
    if (p->cfunc_die) {
        if (arena.resources)
            fatal("cfunc created a resource then died — payload would leak");
        const char *reason = p->cfunc_die;
        p->cfunc_die = NULL;
        callback_arena_release(&arena);
        p->ccall = NULL;
        proc_gc_leave(p);
        rt_die(p, reason);
    }
    if (p->yield_requested)
        fatal("cfunc yield is not supported by the C backend yet (M2)");
    callback_arena_converge(p, &r);
    p->ccall = NULL;
    /* 结果先入槽（根集），重开后再从槽读：重开可能收集，搬移后 C 局部的 r
     * 是旧地址。 */
    RT_SLOT(p, at_slot) = r;
    proc_gc_reopen(p);
    return RT_SLOT(p, at_slot);
}

/* ---- halt：续延链的终点 ----
 *
 * base==0 的帧是 entry 帧（没有任何调用者）。entry 返回 = 程序结束；
 * 非 entry 帧返回 halt 只可能是 cgen 的 bug（halt 只写在 entry 帧的
 * RT_CONT 里）。 */
Val rt_stage_halt(Proc *p, Val a1, Val a2, Val a3, int base, long *bg) {
    (void)a2, (void)a3, (void)bg;
    VM *vm = rt_tls_vm;
    if (base != 0)
        fatal("return past the entry frame");
    if (p->pid == vm->main_pid) {
        vm->exit_val = a1;
        atomic_store(&vm->entry_done, 1);
    } else {
        p->sp = -(base + RT_ARGS);
        proc_die(vm, p, val_nil());
    }
    return val_nil();
}

/* ---- 运行循环的一片 ----
 *
 * 与 vm.c 的 run_proc 同契约：每片返回 R_DIED / R_BLOCKED / R_YIELD，
 * 入队/摘除由 scheduler.c 的 worker_loop 按状态决定（解释器自身永不入队）。
 * 结局由 Proc 状态表达，不新增字段：
 *   PROC_DEAD                        → R_DIED（proc_die 已摘）
 *   PROC_RUNNING + entry_done + main → R_DIED（entry 返回 = 程序结束）
 *   PROC_WAIT_*                      → R_BLOCKED
 *   其余（预算耗尽）                  → R_YIELD（重新入队，从 pc/base 恢复）
 */
int run_proc(VM *vm, Proc *p) {
    tls_current_proc = p;
    rt_tls_vm = vm;
    /* 预算对齐 scheduler.c 的 MAX_REDUCTIONS；profiling 时加抖动（同 vm.c：
     * 固定 1000 会让同构负载的采样点永远落在同一条指令上）。 */
    long budget = 1000;
    if (vm->prof_on) {
        p->prof_last = prof_now_ns();
        budget += (long)((p->prof_last >> 10) & 0x7FF);
    }

    if (setjmp(rt_die_jb) != 0) {
        rt_die_ready = 0;
        return R_DIED; /* rt_die：现场已发布，proc_die 已跑 */
    }
    rt_die_ready = 1;
    /* 恢复：实参从帧里的"家"槽读回（让出点必然在调用前，那里实参已落槽），
     * 入口帧的架上写的是 nil。 */
    int base = p->base;
    Val a1 = RT_SLOT(p, base + RT_A1);
    Val a2 = RT_SLOT(p, base + RT_A2);
    Val a3 = RT_SLOT(p, base + RT_A3);
    (void)rt_stages[p->pc](p, a1, a2, a3, base, &budget);
    rt_die_ready = 0;

    int st = atomic_load(&p->state);
    if (st == PROC_DEAD)
        return R_DIED;
    if (st != PROC_RUNNING)
        return R_BLOCKED;
    if (p->pid == vm->main_pid && atomic_load(&vm->entry_done))
        return R_DIED;
    /* 预算耗尽：采样边界 = 让出边界（与 vm.c 的 TICK 同款归因）。 */
    if (vm->prof_on) {
        uint64_t now = prof_now_ns();
        prof_collect(vm, p, now - p->prof_last);
        p->prof_last = now;
    }
    return R_YIELD;
}

/* ---- 宿主 main ----
 *
 * entry proc 的帧（base 0）在这里手工建：self=nil（entry 不是闭包调用）、
 * cont=halt、cbase=0、其余槽 nil，然后交给共享的 vm_run（scheduler.c）。
 * 退出码/输出与字节码宿主一致：main 崩溃 → 1，否则打印 exit_val + "\n"。 */
int main(int argc, char **argv) {
    (void)argc, (void)argv;
    VM *vm = ta_host_init();
    Proc *entry = proc_new(vm);
    if (!entry)
        fatal("proc_new failed");
    tls_current_proc = entry;
    vm->main_pid = entry->pid;
    proc_ensure_heap(entry);
    rt_init_globals(vm);

    proc_stack_reserve(entry, -(RT_ARGS));
    for (int i = 0; i < RT_ARGS; i++)
        RT_SLOT(entry, i) = val_nil();
        RT_SLOT(entry, RT_CONT) = val_int((int64_t)RT_STAGE_HALT);
    /* cb 也必须是"合法 Val 且等于 0"：entry 帧自己的 RT_RETURN 会读它
     * （读的是 base 0 的头）。留 nil 就等于把恢复点交给 val_get_int(nil)。 */
    RT_SLOT(entry, RT_CBASE) = val_int(0);
    entry->base = 0;
    entry->pc = rt_entry_stage;
    entry->sp = -RT_ARGS;

    vm_run(vm, entry);
    print_val(vm, vm->exit_val);
    printf("\n");
    fflush(stdout);
    return atomic_load(&vm->main_crashed) ? 1 : 0;
}