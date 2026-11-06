/* rt_smoke.c — 手写的 ABI 冒烟程序：cgen.ta（M1）必须生成的形状就是它。
 *
 * KL 源：
 *   (define fib (lambda (n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2))))))
 *   (define make-adder (lambda (k) (lambda (x) (+ x k))))
 *   (define loop (lambda (i acc) (if (= i 0) acc (loop (- i 1) (+ acc i)))))
 *   (define build (lambda (i acc) (if (= i 0) acc (build (- i 1) (cons i acc)))))
 *   (define sum (lambda (l acc) (if (null? l) acc (sum (cdr l) (+ acc (car l))))))
 *   (define p4 (lambda (a b c d) (cons a (cons b (cons c (cons d nil))))))
 *   (define use4 (lambda (x) (cons x (p4 1 2 3 4))))
 *   (cons (cons (fib 20) (cons (loop 1000000 0) (cons ((make-adder 40) 2)
 *               (cons (float? 1.5) (cons (sum (build 1000 nil) 0) nil)))))
 *         (cons (use4 9) (cons (= 2 2.0) nil)))
 *
 * 覆盖的机制（每一格都是设计里唯一的说法，不能靠"大概"）：
 *   - 非尾调用 + 续延 stage（fib 的两处递归：cont 在帧内固定偏移，无需搬）
 *   - 尾调用（loop 100 万次：复用帧、零增长、预算耗尽后从"家"槽恢复实参）
 *   - 堆分配跨 safepoint（cons 链：操作数在槽里，分配后从槽读）
 *   - 带自由变量的闭包（make-adder：HeapClosure.free + 动态 stage 分发）
 *   - C 模块调用（float?：窗口协议 + 结果从槽重读）
 *   - 分配 + 尾调用：build 的 acc 跨 cons 的 safepoint（TA_GC_STRESS=1 下
 *     1000 次搬移 GC，列表必须整体活着 —— 这是 spill 纪律的硬测）
 *   - 尾递归零 spill：sum 的 depth = 0（尾调用不需要任何 spill 槽）
 *   - 溢出形参（ov）：p4 的 4 个形参 —— 第 4 个落在 base+RT_ARGS 的家槽，
 *     由调用者写；本帧 spill 从 RT_ARGS+1 起，fill 不能碰它
 *   - 多实参调用（nov）：(p4 1 2 3 4) 的第 4 个实参写 nb+RT_ARGS，调用者
 *     必须在 SETUP 时发布到那个深度（否则被调 fill 扩栈搬丢）
 *   - 数值等价：= 走 cmp_numeric_path（2 == 2.0 为真），不是按位比较
 *   - self 经槽传递、cont/cbase 经槽恢复
 *
 * 帧布局：0 self｜1 cont｜2 cbase｜3 保留｜4..6 a1..a3 的"家"｜7.. spill。
 * depth = 本函数 spill 槽数；调用点 nb = base + RT_ARGS + 该点仍活的槽数。
 */
#include "../src/rt.h"

static int g_cf_floatp;

enum {
    S_HALT = 0, /* rt_stage_halt（rt.c 提供，恒为 0） */
    S_ENTRY,
    E_CT0,
    E_CT1,
    E_CT2,
    E_CT3,
    E_CT4,
    E_CT5,
    S_FIB,
    F_CT0,
    F_CT1,
    S_LOOP,
    S_MKADDER,
    S_ADDER,
    S_BUILD,
    S_SUM,
    S_P4,
    S_USE4,
    U4_CT0,
    E_CT6,
};

static Val st_entry(Proc *p, Val a1, Val a2, Val a3, int base, long *bg);
static Val e_ct0(Proc *p, Val r, Val a2, Val a3, int base, long *bg);
static Val e_ct1(Proc *p, Val r, Val a2, Val a3, int base, long *bg);
static Val e_ct2(Proc *p, Val r, Val a2, Val a3, int base, long *bg);
static Val e_ct3(Proc *p, Val r, Val a2, Val a3, int base, long *bg);
static Val e_ct4(Proc *p, Val r, Val a2, Val a3, int base, long *bg);
static Val e_ct5(Proc *p, Val r, Val a2, Val a3, int base, long *bg);
static Val st_fib(Proc *p, Val n, Val a2, Val a3, int base, long *bg);
static Val f_ct0(Proc *p, Val r1, Val a2, Val a3, int base, long *bg);
static Val f_ct1(Proc *p, Val r2, Val a2, Val a3, int base, long *bg);
static Val st_loop(Proc *p, Val i, Val acc, Val a3, int base, long *bg);
static Val st_mkadder(Proc *p, Val k, Val a2, Val a3, int base, long *bg);
static Val st_adder(Proc *p, Val x, Val a2, Val a3, int base, long *bg);
static Val st_build(Proc *p, Val i, Val acc, Val a3, int base, long *bg);
static Val st_sum(Proc *p, Val l, Val acc, Val a3, int base, long *bg);
static Val st_p4(Proc *p, Val a, Val b, Val c, int base, long *bg);
static Val st_use4(Proc *p, Val x, Val a2, Val a3, int base, long *bg);
static Val u4_ct0(Proc *p, Val r, Val a2, Val a3, int base, long *bg);
static Val e_ct6(Proc *p, Val r, Val a2, Val a3, int base, long *bg);

RtFn const rt_stages[] = {
    rt_stage_halt, st_entry, e_ct0,      e_ct1,    e_ct2,    e_ct3,  e_ct4, e_ct5,   st_fib, f_ct0,
    f_ct1,         st_loop,  st_mkadder, st_adder, st_build, st_sum, st_p4, st_use4, u4_ct0, e_ct6,
};
const int rt_stage_count = (int)(sizeof rt_stages / sizeof rt_stages[0]);
const int rt_entry_stage = S_ENTRY;

void rt_init_globals(VM *vm) { g_cf_floatp = rt_cfunc(vm, "float?"); }

/* (define fib (lambda (n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))) */
static Val st_fib(Proc *p, Val n, Val a2, Val a3, int base, long *bg) {
    (void)a2, (void)a3;
    RT_ENTER(bg);
    RT_FILL(p, base, 0, 2); /* 槽 7=n, 8=r1 */
    if (val_is_true(rt_lt(n, val_int(2))))
        RT_RETURN(p, n, base, bg);
    RT_SLOT(p, base + 7) = n; /* n 跨两次递归调用 */
    {
        Val m = rt_sub(n, val_int(1));
        int nb = base + RT_ARGS + 1; /* 调用点仍活：n */
        RT_SETUP_FRAME(p, nb, RT_SLOT(p, base + RT_SELF), F_CT0, base, 0);
        if (*bg <= 0)
            RT_YIELD_CALL(p, S_FIB, nb, m, val_nil(), val_nil());
        RT_MUSTTAIL return st_fib(p, m, val_nil(), val_nil(), nb, bg);
    }
}

static Val f_ct0(Proc *p, Val r1, Val a2, Val a3, int base, long *bg) {
    (void)a2, (void)a3;
    RT_ENTER(bg);
    RT_SP(p, base, 2); /* 续延接管本帧：把 sp 收回本帧覆盖范围 */
    RT_SLOT(p, base + 8) = r1;
    {
        Val m = rt_sub(RT_SLOT(p, base + 7), val_int(2));
        int nb = base + RT_ARGS + 2; /* 仍活：n, r1 */
        RT_SETUP_FRAME(p, nb, RT_SLOT(p, base + RT_SELF), F_CT1, base, 0);
        if (*bg <= 0)
            RT_YIELD_CALL(p, S_FIB, nb, m, val_nil(), val_nil());
        RT_MUSTTAIL return st_fib(p, m, val_nil(), val_nil(), nb, bg);
    }
}

static Val f_ct1(Proc *p, Val r2, Val a2, Val a3, int base, long *bg) {
    (void)a2, (void)a3;
    RT_ENTER(bg);
    RT_SP(p, base, 2);
    RT_RETURN(p, rt_add(RT_SLOT(p, base + 8), r2), base, bg);
}

/* (define loop (lambda (i acc) (if (= i 0) acc (loop (- i 1) (+ acc i))))) */
static Val st_loop(Proc *p, Val i, Val acc, Val a3, int base, long *bg) {
    (void)a3;
    RT_ENTER(bg);
    RT_FILL(p, base, 0, 2);
    if (val_is_true(rt_eq(i, val_int(0))))
        RT_RETURN(p, acc, base, bg);
    {
        /* 尾调用：base 复用、cont 原地继承 —— 整个调用零帧开销。 */
        Val i2 = rt_sub(i, val_int(1));
        Val acc2 = rt_add(acc, i);
        if (*bg <= 0)
            RT_YIELD_CALL(p, S_LOOP, base, i2, acc2, val_nil());
        RT_MUSTTAIL return st_loop(p, i2, acc2, val_nil(), base, bg);
    }
}

/* (define make-adder (lambda (k) (lambda (x) (+ x k)))) */
static Val st_mkadder(Proc *p, Val k, Val a2, Val a3, int base, long *bg) {
    (void)a2, (void)a3;
    RT_ENTER(bg);
    RT_FILL(p, base, 0, 1); /* 槽 7=k（分配前必须在根集里） */
    RT_SLOT(p, base + 7) = k;
    RT_RETURN(p, rt_mkclos(p, S_ADDER, 1, base + 7), base, bg);
}

static Val st_adder(Proc *p, Val x, Val a2, Val a3, int base, long *bg) {
    (void)a2, (void)a3;
    RT_ENTER(bg);
    RT_FILL(p, base, 0, 0); /* 无 safepoint：k 留在寄存器 */
    Val self = RT_SLOT(p, base + RT_SELF);
    Val k = ((HeapClosure *)(uintptr_t)lpayload(self))->free[0];
    RT_RETURN(p, rt_add(x, k), base, bg);
}

/* entry：把六个结果 cons 成一张表。 */
static Val st_entry(Proc *p, Val a1, Val a2, Val a3, int base, long *bg) {
    (void)a1, (void)a2, (void)a3;
    RT_ENTER(bg);
    RT_FILL(p, base, 0, 13); /* 7=fib 8=loop 9=adder 10=adder 结果 11=build 12=sum
                              * 13..14=cons 15=(use4 9) 16..19=尾段 */
    {
        int nb = base + RT_ARGS;
        RT_SETUP_FRAME(p, nb, RT_CLOS(S_FIB), E_CT0, base, 0);
        if (*bg <= 0)
            RT_YIELD_CALL(p, S_FIB, nb, val_int(20), val_nil(), val_nil());
        RT_MUSTTAIL return st_fib(p, val_int(20), val_nil(), val_nil(), nb, bg);
    }
}

static Val e_ct0(Proc *p, Val r, Val a2, Val a3, int base, long *bg) {
    (void)a2, (void)a3;
    RT_ENTER(bg);
    RT_SP(p, base, 13);
    RT_SLOT(p, base + 7) = r;
    {
        int nb = base + RT_ARGS + 1;
        RT_SETUP_FRAME(p, nb, RT_CLOS(S_LOOP), E_CT1, base, 0);
        if (*bg <= 0)
            RT_YIELD_CALL(p, S_LOOP, nb, val_int(1000000), val_int(0), val_nil());
        RT_MUSTTAIL return st_loop(p, val_int(1000000), val_int(0), val_nil(), nb, bg);
    }
}

static Val e_ct1(Proc *p, Val r, Val a2, Val a3, int base, long *bg) {
    (void)a2, (void)a3;
    RT_ENTER(bg);
    RT_SP(p, base, 13);
    RT_SLOT(p, base + 8) = r;
    {
        int nb = base + RT_ARGS + 2;
        RT_SETUP_FRAME(p, nb, RT_CLOS(S_MKADDER), E_CT2, base, 0);
        if (*bg <= 0)
            RT_YIELD_CALL(p, S_MKADDER, nb, val_int(40), val_nil(), val_nil());
        RT_MUSTTAIL return st_mkadder(p, val_int(40), val_nil(), val_nil(), nb, bg);
    }
}

/* ((make-adder 40) 2)：闭包值在运行时才知道，stage id 从闭包头取。 */
static Val e_ct2(Proc *p, Val clos, Val a2, Val a3, int base, long *bg) {
    (void)a2, (void)a3;
    RT_ENTER(bg);
    RT_SP(p, base, 13);
    RT_SLOT(p, base + 9) = clos;
    {
        int sid = rt_stage_of(clos);
        int nb = base + RT_ARGS + 3;
        RT_SETUP_FRAME(p, nb, clos, E_CT3, base, 0);
        if (*bg <= 0)
            RT_YIELD_CALL(p, sid, nb, val_int(2), val_nil(), val_nil());
        RT_MUSTTAIL return rt_stages[sid](p, val_int(2), val_nil(), val_nil(), nb, bg);
    }
}

static Val e_ct3(Proc *p, Val r, Val a2, Val a3, int base, long *bg) {
    (void)a2, (void)a3;
    RT_ENTER(bg);
    RT_SP(p, base, 13);
    RT_SLOT(p, base + 10) = r;
    {
        int nb = base + RT_ARGS + 4;
        RT_SETUP_FRAME(p, nb, RT_CLOS(S_BUILD), E_CT4, base, 0);
        if (*bg <= 0)
            RT_YIELD_CALL(p, S_BUILD, nb, val_int(1000), val_nil(), val_nil());
        RT_MUSTTAIL return st_build(p, val_int(1000), val_nil(), val_nil(), nb, bg);
    }
}

static Val e_ct4(Proc *p, Val r, Val a2, Val a3, int base, long *bg) {
    (void)a2, (void)a3;
    RT_ENTER(bg);
    RT_SP(p, base, 13);
    RT_SLOT(p, base + 11) = r;
    {
        int nb = base + RT_ARGS + 5;
        RT_SETUP_FRAME(p, nb, RT_CLOS(S_SUM), E_CT5, base, 0);
        if (*bg <= 0)
            RT_YIELD_CALL(p, S_SUM, nb, r, val_int(0), val_nil());
        RT_MUSTTAIL return st_sum(p, r, val_int(0), val_nil(), nb, bg);
    }
}

static Val e_ct5(Proc *p, Val r, Val a2, Val a3, int base, long *bg) {
    (void)a2, (void)a3;
    RT_ENTER(bg);
    RT_SP(p, base, 13);
    RT_SLOT(p, base + 12) = r;
    /* (float? 1.5)：cfunc 调用，实参经槽（窗口协议下 C 数组副本安全）。
     * 结果由 rt_ccall_at 写回 at_slot，所以调用后从槽读、不接返回值。 */
    RT_SLOT(p, base + 13) = val_float(1.5);
    rt_ccall_at(p, g_cf_floatp, base + 13, 1); /* → slot 13 */
    /* cons 链：每一步都是 safepoint，操作数从槽读；槽只复用 14 一处。
     * 不能指望"入口 fill 过所以槽还是 nil" —— 被调帧就落在这些槽上
     * （nb = base+RT_ARGS+调用点活跃数），返回后槽里是上一帧的残留。 */
    RT_SLOT(p, base + 14) = val_nil();
    Val c = rt_cons_at(p, base + 13, base + 14); /* (float? . nil) */
    RT_SLOT(p, base + 14) = c;
    c = rt_cons_at(p, base + 12, base + 14); /* (sum . …) */
    RT_SLOT(p, base + 14) = c;
    c = rt_cons_at(p, base + 10, base + 14); /* (adder . …) */
    RT_SLOT(p, base + 14) = c;
    c = rt_cons_at(p, base + 8, base + 14); /* (loop . …) */
    RT_SLOT(p, base + 14) = c;
    RT_SLOT(p, base + 15) = rt_cons_at(p, base + 7, base + 14);
    /* (use4 9)：nov=0、非尾 —— 五个结果跨这次调用，全在槽里 */
    {
        int nb = base + RT_ARGS + 9;
        RT_SETUP_FRAME(p, nb, RT_CLOS(S_USE4), E_CT6, base, 0);
        if (*bg <= 0)
            RT_YIELD_CALL(p, S_USE4, nb, val_int(9), val_nil(), val_nil());
        RT_MUSTTAIL return st_use4(p, val_int(9), val_nil(), val_nil(), nb, bg);
    }
}

/* (define p4 (lambda (a b c d) (cons a (cons b (cons c (cons d nil))))))
 *
 * ov=1：第 4 个形参在 base+RT_ARGS（调用者写的），本帧 spill 从 RT_ARGS+1
 * 起。cons 是 safepoint，所以 a/b/c 这三个寄存器形参也要落槽 —— 分配后从槽
 * 读，C 局部副本会悬垂。 */
static Val st_p4(Proc *p, Val a, Val b, Val c, int base, long *bg) {
    RT_ENTER(bg);
    RT_FILL(p, base, 1, 4); /* 8=a 9=b 10=c 11=cur */
    RT_SLOT(p, base + 8) = a;
    RT_SLOT(p, base + 9) = b;
    RT_SLOT(p, base + 10) = c;
    RT_SLOT(p, base + 11) = val_nil();
    RT_SLOT(p, base + 11) = rt_cons_at(p, base + RT_ARGS, base + 11); /* (d . nil) */
    RT_SLOT(p, base + 11) = rt_cons_at(p, base + 10, base + 11);
    RT_SLOT(p, base + 11) = rt_cons_at(p, base + 9, base + 11);
    RT_RETURN(p, rt_cons_at(p, base + 8, base + 11), base, bg);
}

/* (define use4 (lambda (x) (cons x (p4 1 2 3 4))))
 * 非尾 4 实参调用（nov=1）：第 4 个实参写 nb+RT_ARGS；x 跨这次调用，落槽。 */
static Val st_use4(Proc *p, Val x, Val a2, Val a3, int base, long *bg) {
    (void)a2, (void)a3;
    RT_ENTER(bg);
    RT_FILL(p, base, 0, 2); /* 7=x 8=r */
    RT_SLOT(p, base + 7) = x;
    {
        int nb = base + RT_ARGS + 1; /* 调用点仍活：x */
        RT_SETUP_FRAME(p, nb, RT_CLOS(S_P4), U4_CT0, base, 1);
        RT_SLOT(p, nb + RT_ARGS) = val_int(4); /* nov=1：第 4 个实参 */
        if (*bg <= 0)
            RT_YIELD_CALL(p, S_P4, nb, val_int(1), val_int(2), val_int(3));
        RT_MUSTTAIL return st_p4(p, val_int(1), val_int(2), val_int(3), nb, bg);
    }
}

static Val u4_ct0(Proc *p, Val r, Val a2, Val a3, int base, long *bg) {
    (void)a2, (void)a3;
    RT_ENTER(bg);
    RT_SP(p, base, 2);
    RT_SLOT(p, base + 8) = r;
    RT_RETURN(p, rt_cons_at(p, base + 7, base + 8), base, bg);
}

/* E_CT6：(= 2 2.0) 走 cmp_numeric_path ⇒ 真（与 vm.c 的 op_eq 同一判定，
 * 按位比较会得假）。cons 到表头，收尾。 */
static Val e_ct6(Proc *p, Val r, Val a2, Val a3, int base, long *bg) {
    (void)a2, (void)a3;
    RT_ENTER(bg);
    RT_SP(p, base, 13);
    RT_SLOT(p, base + 16) = r;
    RT_SLOT(p, base + 17) = val_int(2);
    RT_SLOT(p, base + 18) = val_float(2.0);
    RT_SLOT(p, base + 17) = rt_eq(RT_SLOT(p, base + 17), RT_SLOT(p, base + 18));
    RT_SLOT(p, base + 19) = val_nil();
    RT_SLOT(p, base + 19) = rt_cons_at(p, base + 17, base + 19); /* (= 2 2.0) */
    RT_SLOT(p, base + 19) = rt_cons_at(p, base + 16, base + 19); /* ((use4 9) . …) */
    RT_RETURN(p, rt_cons_at(p, base + 15, base + 19), base, bg);
}

/* (define build (lambda (i acc) (if (= i 0) acc (build (- i 1) (cons i acc)))))
 * acc 跨 cons 的 safepoint，所以它和 i 都在槽里；cons 之后从槽读（分配可能
 * 搬走堆，C 局部副本会悬垂）。 */
static Val st_build(Proc *p, Val i, Val acc, Val a3, int base, long *bg) {
    (void)a3;
    RT_ENTER(bg);
    RT_FILL(p, base, 0, 2); /* 7=acc 8=i */
    if (val_is_true(rt_eq(i, val_int(0))))
        RT_RETURN(p, acc, base, bg);
    RT_SLOT(p, base + 7) = acc;
    RT_SLOT(p, base + 8) = i;
    RT_SLOT(p, base + 7) = rt_cons_at(p, base + 8, base + 7); /* (i . acc) */
    {
        Val i2 = rt_sub(RT_SLOT(p, base + 8), val_int(1));
        Val lst = RT_SLOT(p, base + 7);
        if (*bg <= 0)
            RT_YIELD_CALL(p, S_BUILD, base, i2, lst, val_nil());
        RT_MUSTTAIL return st_build(p, i2, lst, val_nil(), base, bg);
    }
}

/* (define sum (lambda (l acc) (if (null? l) acc (sum (cdr l) (+ acc (car l))))))
 * 纯尾递归：car/cdr 不分配，所以一个 spill 槽都不需要（depth = 0）。 */
static Val st_sum(Proc *p, Val l, Val acc, Val a3, int base, long *bg) {
    (void)a3;
    RT_ENTER(bg);
    RT_FILL(p, base, 0, 0);
    if (l == val_nil())
        RT_RETURN(p, acc, base, bg);
    {
        Val h = rt_car(l);
        Val t = rt_cdr(l);
        Val acc2 = rt_add(acc, h);
        if (*bg <= 0)
            RT_YIELD_CALL(p, S_SUM, base, t, acc2, val_nil());
        RT_MUSTTAIL return st_sum(p, t, acc2, val_nil(), base, bg);
    }
}
