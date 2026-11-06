/* rt.h — KL→C 生成代码的运行时契约（cgen.ta 的 C 侧对端）。
 *
 * 设计见 docs/c-backend.md。三条不变量：
 *
 * 1. 调用约定统一：每个 KL 函数（以及每个续延）编译成一个"stage"：
 *        Val f(Proc *p, Val a1, Val a2, Val a3, int base, long *budget);
 *    前三个实参走 C 实参（寄存器），第 4 个起落帧槽 RT_ARGS 起（溢出区）。
 *    stage 之间一律 musttail —— 原生 C 栈恒为 O(1)，所以"让出/阻塞"只是
 *    把 (stage id, base) 存进 Proc 后 return，唤醒时从槽里重建实参。
 *
 * 2. GC 根集 = Proc 栈 [p->sp, 0)（src/gc.c 唯一的根扫描）。因此跨 safepoint
 *    存活的值必须在槽里：调用/分配之前 spill 到自己的帧，并发布会覆盖它们的
 *    sp。不跨 safepoint 的值留在 C 局部 —— LLVM 分配寄存器，融合掉之后
 *    槽写读会被死代码消除。
 *
 * 3. 帧布局（相对 base，绝对寻址见 RT_SLOT）：
 *        base+RT_SELF   self（本函数闭包；自由变量的宿主，GC 根）
 *        base+RT_CONT   返回目标 stage id
 *        base+RT_CBASE  调用者 base
 *        base+RT_A1..3  a1..a3 的"家"槽：只在让出时落值，恢复时从这里读
 *        base+RT_ARGS   第 4 个起的形参（溢出区，再往上是本函数的 spill 区）
 *    续延的位置与形参个数无关（固定偏移），所以尾调用不需要搬续延
 *    —— 这比字节码 VM 的 base+n+1..3 省一次三槽拷贝。
 */
#ifndef TA_RT_H
#define TA_RT_H

#include "ta.h"
#include "ta_inline.h"

/* nil 是编译期常量（box_tag_payload(TAG_NIL,0)）：RT_FILL 每帧要写 4+ 格，
 * 走 extern 的 val_nil() 就是每次一个函数调用 —— 实测这是热循环里最大的
 * 单笔开销（clang 没法把 val_nil 当常量传播，四个调用一个都省不掉）。 */
#define RT_NIL lbox(TAG_NIL, 0)

/* ---- 帧布局 ---- */
#define RT_SELF 0
#define RT_CONT 1
#define RT_CBASE 2
#define RT_UNUSED 3 /* 保留（曾放 caller sp；续延自己按编译期常量重发布） */
#define RT_A1 4
#define RT_A2 5
#define RT_A3 6
#define RT_ARGS 7
#define RT_MAXREG 3 /* a1..a3 走寄存器；第 4 个起进 RT_ARGS 溢出区 */

/* slot i（0 = 栈底）的绝对地址：与 sp 无关，只取决于 i（vm.c 的 LSTK 同款）。
 * 栈向下长，所以 offset 恒在 [0, mem_size) 内（单个带符号下标避免 UBSan）。 */
#define RT_SLOT(p, i) (*(Val *)((p)->mem + (p)->mem_size - ((int)(i) + 1) * (int)sizeof(Val)))

/* musttail 是这套 ABI 的硬依赖，不是优化开关：stage 之间的调用必须真正
 * 复用 C 帧（原生栈恒 O(1)），而且 stage 里的普通 return（让出/终止）必须
 * 直接落回 run_proc 的帧 —— 少了它，让出会返回到"调用它的那个 stage"并
 * 被当成返回值继续执行，语义全错。GCC 无此属性，故显式炸掉。
 * （tac 的构建目标因此固定用 clang，见 Makefile 的 tac 一节。） */
#if defined(__clang__) && __has_attribute(musttail)
#define RT_MUSTTAIL __attribute__((musttail))
#elif defined(TA_RT_ALLOW_NO_MUSTTAIL)
#define RT_MUSTTAIL /* 仅用于语法检查/格式化，运行不可 */
#else
#error "the KL C backend requires clang's __attribute__((musttail))"
#endif

typedef Val (*RtFn)(Proc *p, Val a1, Val a2, Val a3, int base, long *budget);

/* 顶层函数的"闭包"：TAG_CLOS_ID + 函数表下标，零堆分配（vm.c 的 GLOBAL 同款）。 */
#define RT_CLOS(stage) lbox(TAG_CLOS_ID, (uint64_t)(stage))

/* 生成代码提供：stage 表 + 入口 stage + 全局表初始化。 */
extern RtFn const rt_stages[];
extern const int rt_stage_count;
extern const int rt_entry_stage;
void rt_init_globals(VM *vm);          /* 符号/常量/cfunc 名解析（cgen 生成） */
void rt_reserve_slow(Proc *p, int lo); /* 扩栈冷路径（rt.c） */

/* rt.c 提供：halt stage（续延链终点）与 VM 回指。 */
#define RT_STAGE_HALT 0 /* 生成代码必须把 rt_stage_halt 放在表首 */
Val rt_stage_halt(Proc *p, Val a1, Val a2, Val a3, int base, long *budget);
VM *rt_vm(void);

/* ---- 表达式原语：语义逐条对齐 vm.c 的 op_* / ARITH（int∧int 保持 int，
 * 其余走 double；val_to_double 把非数值降为 0.0）。写成 inline 函数而非宏，
 * 让 LLVM 内联后把整条链融成寄存器运算。 ---- */
static inline Val rt_add(Val a, Val b) {
    if (val_is_int(a) && val_is_int(b))
        return val_int(val_get_int(a) + val_get_int(b));
    return val_from_double(val_to_double(a) + val_to_double(b));
}
static inline Val rt_sub(Val a, Val b) {
    if (val_is_int(a) && val_is_int(b))
        return val_int(val_get_int(a) - val_get_int(b));
    return val_from_double(val_to_double(a) - val_to_double(b));
}
static inline Val rt_mul(Val a, Val b) {
    if (val_is_int(a) && val_is_int(b))
        return val_int(val_get_int(a) * val_get_int(b));
    return val_from_double(val_to_double(a) * val_to_double(b));
}
/* 除零在字节码 VM 里是进程隔离死亡（PROC_DIE_ISOLATED "divzero"），
 * C 后端走同一个出口（rt_die 定义在 rt.c，发布现场后 proc_die）。 */
_Noreturn void rt_die(Proc *p, const char *reason);
static inline Val rt_div(Val a, Val b) {
    if (val_is_int(a) && val_is_int(b)) {
        int64_t d = val_get_int(b);
        if (d == 0)
            rt_die(tls_current_proc, "divzero");
        return val_int(val_get_int(a) / d);
    }
    return val_from_double(val_to_double(a) / val_to_double(b));
}
/* 取模：与 op_mod 逐条一致 —— 非 int∧int 是进程隔离死亡（"arithtype"），
 * 除零同理（issue #158 的语义点，别在 C 后端悄悄拖回浮点除法）。 */
static inline Val rt_mod(Val a, Val b) {
    if (!val_is_int(a) || !val_is_int(b))
        rt_die(tls_current_proc, "arithtype");
    int64_t d = val_get_int(b);
    if (d == 0)
        rt_die(tls_current_proc, "divzero");
    return val_int(val_get_int(a) % d);
}
/* 比较：镜像 vm.c 的 CMP —— cmp_numeric_path（int/float 混合）走 double，
 * int∧int 走整数，其余（含非数值混合）恒 false。 */
#define RT_CMP(fn, oper)                                                                           \
    static inline Val fn(Val a, Val b) {                                                           \
        int r = cmp_numeric_path(a, b)                                                             \
                    ? (val_to_double(a) oper val_to_double(b))                                     \
                    : (val_is_int(a) && val_is_int(b) && (val_get_int(a) oper val_get_int(b)));    \
        return r ? val_true() : val_false();                                                       \
    }
RT_CMP(rt_lt, <)
RT_CMP(rt_le, <=)
RT_CMP(rt_gt, >)
RT_CMP(rt_ge, >=)
#undef RT_CMP
/* eq? /=：cmp_numeric_path 时按 double（3 == 3.0），否则 val_equal
 *（字符串按内容、immediate 按值、heap 按身份）。按位比较会错两处 ——
 * float 与字符串，两处都能在 test 里看出来。 */
static inline Val rt_eq(Val a, Val b) {
    int r = cmp_numeric_path(a, b) ? (val_to_double(a) == val_to_double(b)) : val_equal(a, b);
    return r ? val_true() : val_false();
}

/* car/cdr/pair?/symbol? 的隔离死亡同 vm.c（nil 不是 pair）。 */
static inline Val rt_car(Val v) {
    if (!val_is_pair(v))
        rt_die(tls_current_proc, "cartype");
    return val_get_car(v);
}
static inline Val rt_cdr(Val v) {
    if (!val_is_pair(v))
        rt_die(tls_current_proc, "cdrtype");
    return val_get_cdr(v);
}
static inline Val rt_pairp(Val v) { return val_is_pair(v) ? val_true() : val_false(); }
static inline Val rt_symp(Val v) { return val_is_symbol(v) ? val_true() : val_false(); }

/* 闭包自由变量读取：self 是 base+RT_SELF 的闭包值（nfree>0 ⇒ TAG_CLOS；
 * nfree==0 的闭包编译成 TAG_CLOS_ID，不产生此访问）。 */
static inline Val rt_free(Proc *p, int base, int i) {
    HeapClosure *hc = (HeapClosure *)(uintptr_t)lpayload(RT_SLOT((p), (base) + RT_SELF));
    return hc->free[i];
}

/* ---- 分配：操作数从槽读，分配后再读（proc_heap_alloc 可能 moving GC，
 * 槽会被原地 forward，C 局部副本不会）。 ---- */
Val rt_cons_at(Proc *p, int a_slot, int b_slot);
Val rt_mkclos(Proc *p, int stage, int nfree, int at_slot);
Val rt_ccall_at(Proc *p, int cf, int at_slot, int nargs);
Val rt_const_string(VM *vm, const char *s, int len);
Val rt_const_symbol(VM *vm, const char *s);
int rt_cfunc(VM *vm, const char *name);

/* ---- stage 协议 ---- */

/* 入口：只减预算，不分支。让出点放在 safepoint（那里状态恰好在槽里）。 */
#define RT_ENTER(bg) (--*(bg))

/* safepoint 处的让出：发布恢复点后普通 return（C 栈上是 run_proc 的帧，
 * 因为整条链是 musttail 过来的）。调用者必须先 spill 到 [0, sp_end)。 */
#define RT_YIELD(p, sid, nb)                                                                       \
    do {                                                                                           \
        (p)->pc = (sid);                                                                           \
        (p)->base = (nb);                                                                          \
        return RT_NIL;                                                                             \
    } while (0)

/* 返回：从本帧读续延（2 次装载）+ 间接 musttail。 */
#define RT_RETURN(p, r, b, bg)                                                                     \
    do {                                                                                           \
        int rt_cid = (int)val_get_int(RT_SLOT((p), (b) + RT_CONT));                                \
        int rt_cb = (int)val_get_int(RT_SLOT((p), (b) + RT_CBASE));                                \
        RT_MUSTTAIL return rt_stages[rt_cid]((p), (r), RT_NIL, RT_NIL, (rt_cb), (bg));             \
    } while (0)

/* 动态调用：闭包值 → stage id。TAG_CLOS_ID 是零堆分配的全局函数引用。 */
static inline int rt_stage_of(Val fv) {
    if (val_tag(fv) == TAG_CLOS_ID)
        return (int)lpayload(fv);
    return ((HeapClosure *)(uintptr_t)lpayload(fv))->entry;
}

/* 帧发布：sp 只深不浅。一浅一深来回摆动会让祖先帧的槽在中间那次 GC 里
 * 落在根集外，而下一帧又把 sp 推深、把它们重新纳入扫描 —— 那时它们指向的
 * 是上一轮 fromspace 的残骸（gc_copyable 只查范围，读的是垃圾头）。 */
#define RT_PUBLISH(p, n)                                                                           \
    do {                                                                                           \
        if (-(n) < (p)->sp)                                                                        \
            (p)->sp = -(n);                                                                        \
    } while (0)

/* 帧入口：把 [base+RT_UNUSED, base+RT_ARGS) 与 [base+RT_ARGS+ov, +spill) 填
 * nil 并发布 sp。ov = 溢出形参个数：它们在 base+RT_ARGS.. 上是**形参本身**
 *（调用者写的），本帧不能覆盖。
 *
 * 为什么填 nil：p->sp 是单一深度，GC 扫 [p->sp,0) 里**每一格**。分支各自
 * spill 到不同槽时，合并点上总有一格在本路径上没写过 —— 那里的残留是上一
 * 帧的 Val，可能指向已经压缩掉的对象（fromspace 内非对象边界 = 垃圾头）。
 * 填 nil 让"发布整个帧"这件事与路径无关，代价是每帧 ov+spill+4 次存。
 *
 * 值是否进槽只由"是否跨 safepoint"决定：不跨的值 LLVM 留在寄存器里，
 * 跨的值才 spill —— 这不是"所有局部在槽里"。 */
/* 扩栈预留的快路径内联、慢路径在 rt.c。proc_stack_reserve 本身是 static
 * inline，但函数体大到 clang 拒绝把它内联进每个 stage 的 RT_FILL —— 生成的
 * 热循环里每次迭代一次真调用（实测：去掉它和下面的 RT_NIL，collatz 的
 * 每步开销从 ~44 周期降到 ~20）。真正必须在快路径上的只有两件事：
 * stack_floor 单调下压，和与堆顶的 gap 检查；扩栈（realloc + memmove）是冷
 * 路径，交给慢函数，它把 proc_stack_reserve 整套语义重做一遍。 */
static inline void rt_reserve(Proc *p, int lo) {
    if (lo < p->stack_floor)
        p->stack_floor = lo;
    if (p->mem_size + lo * (int)sizeof(Val) < p->heap_ptr)
        rt_reserve_slow(p, lo);
}

#define RT_FILL(p, b, ov, spill)                                                                   \
    do {                                                                                           \
        rt_reserve((p), -((b) + RT_ARGS + (ov) + (spill)));                                        \
        for (int rt_i = RT_UNUSED; rt_i < RT_ARGS; rt_i++)                                         \
            RT_SLOT((p), (b) + rt_i) = RT_NIL;                                                     \
        for (int rt_i = 0; rt_i < (spill); rt_i++)                                                 \
            RT_SLOT((p), (b) + RT_ARGS + (ov) + rt_i) = RT_NIL;                                    \
        RT_PUBLISH((p), (b) + RT_ARGS + (ov) + (spill));                                           \
    } while (0)

/* 续延 stage 的入口：只把 sp 收回到本帧的覆盖范围（不填、不 reserve）。
 *
 * 为什么要收：sp 只增不减（fill/setup 一路往深处发布），一帧返回后如果没人
 * 收回来，整条最深的死栈会永远落在根集里 —— 语义不错（那些格子里的残留都
 * 是合法 Val），但每次 GC 都要扫它们、并把死对象复制活，长循环里就是持续
 * 泄漏。续延刚刚接管本帧，此刻没有任何 safepoint，所以"sp 太深"这段窗口
 * 无害；收回后覆盖的格子全都合法（本帧入口的 fill 已经把整帧填成 nil，
 * 被调帧覆盖进去的又都是它自己写的合法 Val）。 */
#define RT_SP(p, b, depth) ((p)->sp = -((b) + RT_ARGS + (depth)))

/* 非尾调用：被调帧起于 base + RT_ARGS + rt_fd（rt_fd = 本 stage 整帧大小，
 * 编译期常量），写 self/cont/cbase 后 musttail 进被调。被调入口的 RT_FILL
 * 覆盖它自己的 [nb+3, ...)，不碰这三格。nov = 溢出实参个数：它们由调用者
 * 写在 nb+RT_ARGS.. 上，所以发布必须盖住它们（否则被调 RT_FILL 扩栈时
 * memmove 会把它们搬丢）。
 *
 * 两处不是可选的装饰：
 *  - proc_stack_reserve 在**写头之前**：扩栈=realloc+memmove（不是 GC），
 *    搬后槽号不变但缓冲区换了地方，所以先要地方再写；反过来就是写野指针。
 *  - 头写完把 sp 发布到恰好盖住它们：被调自己的 RT_FILL 若要扩栈，
 *    memmove 只保留 [sp,0)，头在 sp 之下就会被搬丢 —— 少了这一句，
 *    深递归第一次扩栈就会把续延/调用者 base 变成垃圾。 */
#define RT_SETUP_FRAME(p, nb, fv, sid, cb, nov)                                                    \
    do {                                                                                           \
        rt_reserve((p), -((nb) + RT_ARGS + (nov)));                                                \
        RT_SLOT((p), (nb) + RT_SELF) = (fv);                                                       \
        RT_SLOT((p), (nb) + RT_CONT) = val_int((int64_t)(sid));                                    \
        RT_SLOT((p), (nb) + RT_CBASE) = val_int((int64_t)(cb));                                    \
        RT_SLOT((p), (nb) + RT_UNUSED) = RT_NIL;                                                   \
        RT_SLOT((p), (nb) + RT_A1) = RT_NIL;                                                       \
        RT_SLOT((p), (nb) + RT_A2) = RT_NIL;                                                       \
        RT_SLOT((p), (nb) + RT_A3) = RT_NIL;                                                       \
        RT_PUBLISH((p), (nb) + RT_ARGS + (nov));                                                   \
    } while (0)

/* 尾调用：被调复用本帧，溢出实参写在 base+RT_ARGS.. 上。reserve 必须在写
 * 之前（同 RT_SETUP_FRAME 的第一条理由），publish 必须盖住它们（第二条）。
 * 调用的实参此时已经全在 C 局部 q0..qn 里：reserve 可能 memmove 缓冲区，
 * 从槽里"边搬边读"会读到搬走后的旧值。 */
#define RT_TAIL_ARGS(p, b, nov)                                                                    \
    do {                                                                                           \
        rt_reserve((p), -((b) + RT_ARGS + (nov)));                                                 \
        RT_PUBLISH((p), (b) + RT_ARGS + (nov));                                                    \
    } while (0)

/* 让出：恢复点 = 即将进入的那个 stage，所以实参必须落进它的"家"槽
 * （base+RT_A1..A3），run_proc 唤醒时从那里读回 C 实参。冷路径，随便花。 */
#define RT_YIELD_CALL(p, sid, nb, x1, x2, x3)                                                      \
    do {                                                                                           \
        RT_SLOT((p), (nb) + RT_A1) = (x1);                                                         \
        RT_SLOT((p), (nb) + RT_A2) = (x2);                                                         \
        RT_SLOT((p), (nb) + RT_A3) = (x3);                                                         \
        RT_PUBLISH((p), (nb) + RT_ARGS);                                                           \
        RT_YIELD((p), (sid), (nb));                                                                \
    } while (0)

#endif /* TA_RT_H */
