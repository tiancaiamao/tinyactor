/* rt.h — KL→C 生成代码的运行时契约（cgen.ta 的 C 侧对端）。
 *
 * 设计见 docs/c-backend.md。四条不变量：
 *
 * 1. 调用约定统一：每个 KL 函数编译成**一个** C 函数，它的每个 "stage"
 *    （函数入口 + 每处非尾调用之后的续延）是函数内的一个 case：
 *        Val f(Proc *p, Val a1, Val a2, Val a3, int base, long *budget, int pc);
 *    前三个实参走 C 实参（寄存器），第 4 个起落帧槽 RT_SPILL 起（溢出区）。
 *    pc 选出从哪个 stage 进入（switch + goto —— C 没有别的办法从函数外跳进
 *    函数中间）。一律 musttail：原生 C 栈恒为 O(1)，所以"让出/阻塞"只是
 *    把 (stage id, base) 存进 Proc 后 return，唤醒时从槽里重建实参。
 *
 * 2. GC 根集 = Proc 栈 [p->sp, 0)（src/gc.c 唯一的根扫描）。因此跨 safepoint
 *    存活的值必须在槽里：调用/分配之前 spill 到自己的帧，并发布会覆盖它们的
 *    sp。不跨 safepoint 的值留在 C 局部 —— LLVM 分配寄存器，融合掉之后
 *    槽写读会被死代码消除。
 *
 * 3. 帧布局（相对 base，绝对寻址见 RT_SLOT）：
 *        base+RT_SELF   self（本函数闭包；自由变量的宿主，GC 根）
 *        base+RT_CONT   返回目标 = 续延 stage id（INT tag；rt_stages[] 给 (过程, pc)）
 *        base+RT_CBASE  调用者 base
 *        base+RT_SPILL  第 4 个起的形参（溢出区），再往上是本函数的 spill 区
 *    续延的位置与形参个数无关（固定偏移），所以尾调用不需要搬续延
 *    —— 这比字节码 VM 的 base+n+1..3 省一次三槽拷贝。整帧大小 fd 是每个
 *    函数的编译期常量，被调帧永远起于 base + fd（调用者不需要知道被调的 fd）。
 *
 * 4. 常量池：符号/字符串字面量不内联成 rt_const_* 调用，而是进单元的
 *    `static Val *rt_k[]`，在 rt_init_globals 里一次性解析；用点是 `rt_k[i]`。
 *    rt_const_* 会分配（字符串那份 GC 也收不到），求值点上直接调就是每次
 *    执行漏一块内存 + 一次持 sym_lock 的哈希查找 —— 实测占 C 腿 9%。
 */
#ifndef TA_RT_H
#define TA_RT_H

#include "ta.h"
#include "ta_inline.h"

/* nil 是编译期常量（box_tag_payload(TAG_NIL,0)）：rt_fill_frame 每帧要写
 * 若干格，走 extern 的 val_nil() 就是每次一个函数调用 —— 实测这是热循环里
 * 最大的单笔开销（clang 没法把 val_nil 当常量传播，几个调用一个都省不掉）。 */
#define RT_NIL lbox(TAG_NIL, 0)
/* val_true()/val_false() 是 val.c 里的**进程级函数**（不是 inline），而比较
 * 在热循环里每步都出现 —— 每次一个真调用 + 一次返回，实测这是 fib 里剩下的
 * 最大单笔开销。两者返回的就是这两个常量（val.c:66），直接写出来。 */
#define RT_TRUE lbox(TAG_TRUE, 0)
#define RT_FALSE lbox(TAG_FALSE, 0)

/* ---- 帧布局（M2：寄存器传参；协议细节见文件末尾的"stage 协议"）----
 *
 *   0 RT_SELF   1 RT_CONT（续延 stage id）  2 RT_CBASE
 *   3 RT_SPILL ..  溢出形参（>3 个时）+ spill 区；整帧 fd = 3 + ov + nspill
 */
#define RT_SELF 0
#define RT_CONT 1
#define RT_CBASE 2
#define RT_SPILL 3
#define RT_MAXREG 3 /* a1..a3 走寄存器；第 4 个起进 [3, 3+ov) */

/* slot i（0 = 栈底）的绝对地址：与 sp 无关，只取决于 i（vm.c 的 LSTK 同款）。
 * 栈向下长，所以 offset 恒在 [0, mem_size) 内（单个带符号下标避免 UBSan）。 */
/* 槽 i 在本帧里的地址 = stk[-1 - i]。stk 是 (Val *)(mem + mem_size) 的派生
 * 缓存（Proc 里那个字段，唯一写口 proc_set_mem），所以这里是一次 load，而
 * 不是 load mem + load mem_size + 移位 —— 生成的热循环每步有十几次槽访问。 */
#define RT_SLOT(p, i) ((p)->stk[-1 - (int)(i)])

/* 连续几格槽（帧头三格、铺 nil、恢复格三格）用槽指针一次算基址：
 * rt_slotp(p, i)[k] 是槽 i-k，所以写 [i-2, i] 从 rt_slotp(p, i) 起递减。
 * 注意只能在"没有 realloc/GC"的窗口里用 —— 扩栈会换掉 p->mem。 */
static inline Val *rt_slotp(Proc *p, int i) { return p->stk - 1 - i; }

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

typedef Val (*RtFn)(Proc *p, Val a1, Val a2, Val a3, int base, long *budget, int pc);
/* stage id → (过程, 过程内的入口 pc)。一个 KL 函数的入口 stage 与它的续延
 * stage 是同一个 C 函数的不同 case，所以返回/唤醒/动态调用共用一张表。 */
typedef struct RtRet {
    RtFn fn;
    int pc;
} RtRet;

/* 入口 thunk 必须挡住内联：clang 一旦把它折进 stage 体，rt_enter_slow 那次 bl
 * 就留在体内，整个 stage 又得开 C 栈帧、存 6 个 callee-saved —— 冷路径的代价
 * 就摊到热路径上了。 */
#define RT_NOINLINE __attribute__((noinline))

/* 顶层函数的"闭包"：TAG_CLOS_ID + 函数表下标，零堆分配（vm.c 的 GLOBAL 同款）。 */
#define RT_CLOS(stage) lbox(TAG_CLOS_ID, (uint64_t)(stage))

/* C 函数作值：TAG_NATIVE + vm->cfuncs 下标，与 vm.c 的 op_global 同一个表示。
 * 载荷就是 rt_ccall_at 的 cf 实参 —— 所以"调用一个 C 函数值"不需要新的 C
 * 边界协议，调用点只要用 rt_native_cf 把下标取出来。立即值：不进堆，GC 不搬，
 * 可以安全地躺在槽里或 C 局部里跨 safepoint。 */
static inline Val rt_native_val(int cf) { return lbox(TAG_NATIVE, (uint64_t)cf); }
static inline int rt_is_native(Val v) { return val_tag(v) == TAG_NATIVE; }
static inline int rt_native_cf(Val v) { return (int)lpayload(v); }

/* 生成代码提供：stage 表 + 入口 stage + 全局表初始化。 */
extern RtRet const rt_stages[];
extern const int rt_stage_count;
/* 每个 stage 的实参恢复格偏移（= RT_SPILL + 该函数溢出形参个数）。run_proc
 * 唤醒时按 p->pc 查它，把实参读回 C 形参。 */
extern const int rt_resume[];
extern const int rt_entry_stage;
void rt_init_globals(VM *vm);          /* 符号/常量/cfunc 名解析（cgen 生成） */
void rt_reserve_slow(Proc *p, int lo); /* 扩栈冷路径（rt.c） */

/* rt.c 提供：halt stage（续延链终点）与 VM 回指。 */
#define RT_STAGE_HALT 0 /* 生成代码必须把 rt_stage_halt 放在表首 */
Val rt_stage_halt(Proc *p, Val a1, Val a2, Val a3, int base, long *budget, int pc);
VM *rt_vm(void);

/* ---- 表达式原语：语义逐条对齐 vm.c 的 op_* / ARITH（int∧int 保持 int，
 * 其余走 double；val_to_double 把非数值降为 0.0）。写成 inline 函数而非宏，
 * 让 LLVM 内联后把整条链融成寄存器运算。 ---- */
/* int∧int 快路径：3 条位运算，不碰 val_is_int/val_get_int/val_int 三个
 * inline 的调用形态。语义与慢路径**逐位同值**：val_int 就是把 i 的低 48 位
 * 按补码存（ta_inline.h），所以 (a+b)&MASK|TAG 与 val_int((int64_t)(a+b))
 * 是同一个值；负数借位也被 MASK 截在 48 位内。
 * 标量类型是 sexp：KL 的 + - * / 都能吃 float，所以 tag 门是必须的（关掉
 * 它就是 RT_UNCHECKED_ARITH，只在基准/自举内部用）。 */
#define RT_INT_TAG ((uint64_t)TAG_INT << 48)
#define RT_INT_MASK 0x0000FFFFFFFFFFFFULL
#define RT_BOTH_INT(a, b) ((((a) & (b)) >> 48) == (uint64_t)TAG_INT)

static inline Val rt_add_slow(Val a, Val b) {
    if (val_is_int(a) && val_is_int(b))
        return val_int(val_get_int(a) + val_get_int(b));
    return val_from_double(val_to_double(a) + val_to_double(b));
}
static inline Val rt_add(Val a, Val b) {
#ifdef RT_UNCHECKED_ARITH
    return ((a + b) & RT_INT_MASK) | RT_INT_TAG;
#else
    if (RT_BOTH_INT(a, b))
        return ((a + b) & RT_INT_MASK) | RT_INT_TAG;
    return rt_add_slow(a, b);
#endif
}
static inline Val rt_sub_slow(Val a, Val b) {
    if (val_is_int(a) && val_is_int(b))
        return val_int(val_get_int(a) - val_get_int(b));
    return val_from_double(val_to_double(a) - val_to_double(b));
}
static inline Val rt_sub(Val a, Val b) {
#ifdef RT_UNCHECKED_ARITH
    return ((a - b) & RT_INT_MASK) | RT_INT_TAG;
#else
    if (RT_BOTH_INT(a, b))
        return ((a - b) & RT_INT_MASK) | RT_INT_TAG;
    return rt_sub_slow(a, b);
#endif
}
static inline Val rt_mul_slow(Val a, Val b) {
    if (val_is_int(a) && val_is_int(b))
        return val_int(val_get_int(a) * val_get_int(b));
    return val_from_double(val_to_double(a) * val_to_double(b));
}
static inline Val rt_mul(Val a, Val b) {
#ifdef RT_UNCHECKED_ARITH
    return ((a * b) & RT_INT_MASK) | RT_INT_TAG;
#else
    if (RT_BOTH_INT(a, b))
        return ((a * b) & RT_INT_MASK) | RT_INT_TAG;
    return rt_mul_slow(a, b);
#endif
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
/* 比较的快路径同理：int∧int 时 cmp_numeric_path 恒假、走的就是
 * val_get_int 的**整数**比较（不是 double），所以 (int64_t)(a<<16) oper
 * (int64_t)(b<<16) 与慢路径同值，且比两次 val_get_int 便宜。 */
#define RT_CMP(fn, oper)                                                                           \
    static inline Val fn(Val a, Val b) {                                                           \
        if (RT_BOTH_INT(a, b))                                                                     \
            return ((int64_t)((a) << 16) oper(int64_t)((b) << 16)) ? RT_TRUE : RT_FALSE;           \
        int r = cmp_numeric_path(a, b)                                                             \
                    ? (val_to_double(a) oper val_to_double(b))                                     \
                    : (val_is_int(a) && val_is_int(b) && (val_get_int(a) oper val_get_int(b)));    \
        return r ? RT_TRUE : RT_FALSE;                                                             \
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
    return r ? RT_TRUE : RT_FALSE;
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
static inline Val rt_pairp(Val v) { return val_is_pair(v) ? RT_TRUE : RT_FALSE; }
static inline Val rt_symp(Val v) { return val_is_symbol(v) ? RT_TRUE : RT_FALSE; }

/* tuple?/tuple-len/tuple-ref：tuple-ref 无分配，故 inline 进链上；
 * 越界与错型同 car (rt_die：'tupleref 隔离死亡，不静默给 nil)。 */
static inline Val rt_tuplep(Val v) { return val_is_tuple(v) ? RT_TRUE : RT_FALSE; }
static inline Val rt_tuple_len(Val v) {
    if (!val_is_tuple(v))
        rt_die(tls_current_proc, "tuplelentype");
    return val_int(val_tuple_len(v));
}
static inline Val rt_tuple_ref(Val v, Val i) {
    if (!val_is_tuple(v))
        rt_die(tls_current_proc, "tuplereftype");
    if (!val_is_int(i))
        rt_die(tls_current_proc, "tuplerefidx");
    long long k = (long long)val_get_int(i);
    if (k < 0 || k >= (long long)val_tuple_len(v))
        rt_die(tls_current_proc, "tupleref");
    return val_tuple_get(v, (int)k);
}

/* 闭包自由变量读取：self 是 base+RT_SELF 的闭包值（nfree>0 ⇒ TAG_CLOS；
 * nfree==0 的闭包编译成 TAG_CLOS_ID，不产生此访问）。 */
static inline Val rt_free(Proc *p, int base, int i) {
    HeapClosure *hc = (HeapClosure *)(uintptr_t)lpayload(RT_SLOT((p), (base) + RT_SELF));
    return hc->free[i];
}

/* ---- 分配：操作数从槽读，分配后再读（proc_heap_alloc 可能 moving GC，
 * 槽会被原地 forward，C 局部副本不会）。 ---- */
Val rt_cons_at(Proc *p, int a_slot, int b_slot);
/* 实参在连续槽 [at_slot, at_slot+n)（槽数由调用点常量给出，末参也已 spill）
 * —— 一个入口吃掉所有 arity，不像 cons 那样两参分明。 */
Val rt_tuple_at(Proc *p, int at_slot, int n);
Val rt_mkclos(Proc *p, int stage, int nfree, int at_slot);
/* C 函数 / actor 原语的调用点：实参在 [at_slot, at_slot+nargs)，结果写回
 * at_slot。两者都可能阻塞 —— 阻塞时把恢复点发布成 (stage, base)（stage 是
 * cgen 为该调用点单独分配的 site stage，见下方"阻塞站"），sp 发布到 lo，
 * 置 PROC_WAIT_*，返回 RT_NIL。调用点紧跟一句
 *     if (RT_BLOCKED(p)) return RT_NIL;
 * 就地把控制权交回 run_proc。 */
Val rt_ccall_at(Proc *p, int cf, int at_slot, int nargs, int stage, int base, int lo);
/* actor 原语（RTB_* 见下）。不阻塞的几条（self/spawn/send/monitor/
 * recv_commit）走 rt_b_*，永不让出。 */
Val rt_actor_at(Proc *p, int id, int at_slot, int nargs, int stage, int base, int lo);
typedef enum {
    RTB_SELF = 0,
    RTB_SPAWN,
    RTB_SEND,
    RTB_RECV,
    RTB_MONITOR,
    RTB_RECV_AFTER,
    RTB_RECV_PEEK,
    RTB_RECV_COMMIT
} RtBuiltin;
Val rt_b_self(Proc *p);
Val rt_b_spawn(Proc *p, Val clos, int stage, int base, int lo);
Val rt_b_send(Proc *p, Val pidv, Val msg, int stage, int base, int lo);
Val rt_b_recv_commit(Proc *p);
Val rt_const_string(VM *vm, const char *s, int len);
Val rt_const_symbol(VM *vm, const char *s);
int rt_cfunc(VM *vm, const char *name);

/* 阻塞判据：`rt_*_at` 返回后 p->state 已不是 RUNNING（WAIT_RECV / WAIT_IO）
 * —— 与 run_proc 判 R_BLOCKED 的条件同一个，不做第二份真相。 */
#define RT_BLOCKED(p) (atomic_load(&(p)->state) != PROC_RUNNING)

/* ---- stage 协议（M2：寄存器传参 ABI）----
 *
 * 帧布局（每帧 fd = RT_SPILL + ov + nspill，三个分量都是编译期常量）：
 *
 *   0  RT_SELF   闭包值（自由变量从这里取；顶层函数是 TAG_CLOS_ID）
 *   1  RT_CONT   续延 = **stage id**（INT tag）。返回时查 rt_stages[] 得
 *                (过程, pc) 再间接 musttail：一个 KL 函数只有一份过程，
 *                续延是它内部的 case。
 *   2  RT_CBASE  调用者帧（base 下标）
 *   3  .. 溢出形参（>3 个时；形参本身，调用者写）
 *   3+ov .. spill 区（具名绑定的 home 槽 + 匿名中间值）
 *
 * 实参 1..3 走 C 形参 a1..a3（sret 式：真正的寄存器传参），第 4 个起才落
 * 槽。所以"M1 每帧写 a1..a3 家槽再读回"的两趟内存往返整个消失。
 *
 * 让出的恢复点：被调 stage 的入口，实参落在**本帧之外**的 3 格
 * [base+RT_SPILL+ov, +3)（`rt_resume[]` 给 run_proc 查同一个偏移）。冷路径
 * 才会写，热路径不付代价。RT_LINK 预留时多带 3 格就是这个用途。
 *
 * 让出/扩栈一致性由两条不变量保证：
 *  1. p->sp 是单一深度，GC 扫 [p->sp,0) 里每一格 → 被发布的格子必须装合法
 *     Val。入口把整个 spill 区填 nil，任何时刻发布整帧都安全（与路径无关）。
 *  2. safepoint 处活值必须在已发布槽里，C 局部副本在 GC 后作废 → 调用点
 *     顺序是 spill → reserve(可能 GC) → reload → 算实参 → 写帧 → musttail。
 */
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

/* 返回：本帧读续延 stage id（1 次装载）→ rt_stages[] 得 (过程, pc) → musttail。 */
#define RT_RETURN(p, r, b, bg)                                                                     \
    do {                                                                                           \
        RtRet rt_k = rt_stages[(int)val_get_int(RT_SLOT((p), (b) + RT_CONT))];                     \
        int rt_cb = (int)val_get_int(RT_SLOT((p), (b) + RT_CBASE));                                \
        RT_MUSTTAIL return rt_k.fn((p), (r), RT_NIL, RT_NIL, (rt_cb), (bg), rt_k.pc);              \
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

/* 扩栈预留的快路径内联、慢路径在 rt.c。proc_stack_reserve 本身是 static
 * inline，但函数体大到 clang 拒绝把它内联进每个 stage —— 生成的热循环里每次
 * 迭代一次真调用（实测：去掉它，collatz 的每步开销从 ~44 周期降到 ~20）。
 * 真正必须在快路径上的只有两件事：stack_floor 单调下压，和与堆顶的 gap
 * 检查；扩栈（realloc + memmove）是冷路径，交给慢函数，它把
 * proc_stack_reserve 整套语义重做一遍。 */
static inline void rt_reserve(Proc *p, int lo) {
    /* gap 检查只在 floor 真正下压时做：堆侧的 fit 检查（ta_inline.h 的
     * proc_heap_alloc 一族）保守地把堆顶压在 min(sp, stack_floor) 之上，
     * 所以"这一帧不比历史最深帧更深"就意味着它不可能已经和堆撞上 —— 否则
     * 堆那侧早就先撞了。热循环里 floor 只在第一次递归到该深度时变化，
     * 于是这条快路径退化成一次 load+cmp。 */
    if (lo < p->stack_floor)
        p->stack_floor = lo;
    if (p->mem_size + lo * (int)sizeof(Val) < p->heap_ptr)
        rt_reserve_slow(p, lo);
}

/* spill 区铺 nil：[b+RT_SPILL+ov, b+fd)。
 *
 * 为什么整帧铺 nil：p->sp 是单一深度，GC 扫 [p->sp,0) 里**每一格**。分支各自
 * spill 到不同槽时，合并点上总有一格在本路径上没写过 —— 那里的残留是上一帧
 * 的 Val，可能指向已经压缩掉的对象（fromspace 内非对象边界 = 垃圾头）。填 nil
 * 让"发布整个帧"这件事与路径无关，代价是每帧 nspill 次存（clang 会把
 * 常量边界的循环完全展开）。 */
static inline void rt_fill_frame(Proc *p, int b, int fd, int ov) {
    for (int i = ov; i < fd - RT_SPILL; i++)
        RT_SLOT(p, b + RT_SPILL + i) = RT_NIL;
}

/* 扩栈冷路径（由每个 stage 自己的入口 thunk 调用）：实参还在寄存器里，扩栈 =
 * realloc + memmove 或碰撞 GC，两者都会让 C 局部寄存器副本失效。先把 a1..a3
 * 存进本帧的"实参恢复格" [b+RT_SPILL+ov, +3) —— 让开 ov 个溢出形参槽，否则会
 * 覆盖调用者刚铺好的第 4 个起的实参（那几格比扩栈更早落地），发布，扩栈；
 * thunk 再从同一组格子读回实参尾调进 stage 体。 */
void rt_enter_slow(Proc *p, int b, int fd, int ov, Val a1, Val a2, Val a3);

/* 调用者在自己入口就多要这几格 slack，于是
 *  - 调用点写被调帧头 [nb, nb+3) 与溢出实参 [nb+3, nb+3+nov)（nov ≤ 3）时不必
 *    再查空间 —— 这是把 bl 从热路径上摘干净的关键：有一个 bl，clang 就得给整个
 *    stage 分配 C 栈帧、保存 6 个 callee-saved 寄存器；
 *  - 被调入口 thunk 有地方暂存 a1..a3。
 * 这些格子不装长活值（只在 thunk 里短暂装实参并发布），但必须**已被背**：把
 * floor 下压到 -(base+fd+RT_SLACK)，堆侧 fit 检查就会护住它们。 */
#define RT_SLACK (RT_SPILL + 3)

/* 帧入口（只有 cont=false 的 stage 用）：一次 gap 比较；不够就**尾调用**自己的
 * 入口 thunk（thunk 扩栈后带着重新读回的实参尾调进本过程）。热路径不许有非
 * 尾调用 —— 实测 fib30：生成器 17.4ms vs 同样协议手写 9.1ms，差的就是这笔。 */
#define RT_ENTER_FRAME(p, b, fd, ov, thunk, a1, a2, a3)                                            \
    do {                                                                                           \
        if ((p)->mem_size - ((b) + (fd) + RT_SLACK) * (int)sizeof(Val) < (p)->heap_ptr)            \
            RT_MUSTTAIL return thunk((p), (a1), (a2), (a3), (b), (bg), (pc));                      \
        if (-((b) + (fd) + RT_SLACK) < (p)->stack_floor)                                           \
            (p)->stack_floor = -((b) + (fd) + RT_SLACK);                                           \
        rt_fill_frame((p), (b), (fd), (ov));                                                       \
        RT_PUBLISH((p), (b) + (fd));                                                               \
    } while (0)

/* 续延 stage 的入口：只把 sp 收回到本帧的覆盖范围（不填、不 reserve）。
 *
 * 为什么要收：sp 只增不减（发布一路往深处推），一帧返回后如果没人收回来，
 * 整条最深的死栈会永远落在根集里 —— 语义不错（那些格子里的残留都是合法
 * Val），但每次 GC 都要扫它们、并把死对象复制活，长循环里就是持续泄漏。
 * 续延刚刚接管本帧，此刻没有任何 safepoint，所以"sp 太深"这段窗口无害。 */
#define RT_SP(p, b, fd) ((p)->sp = -((b) + (fd)))

/* 非尾调用：被调帧起于 base + rt_fd（rt_fd 是本函数整帧大小，编译期常量），
 * 写 self/cont/cbase 三格头后发布，再由调用点 musttail 进被调。
 *
 * 这里**不查空间、不扩栈**：调用者入口已经多要了 RT_SLACK 格（见 RT_SLACK），
 * 正好盖住帧头 3 格 + 溢出实参（nov ≤ 3）。被调自己的入口检查负责它整帧。
 * 头写完把 sp 发布到恰好盖住它们：被调入口若还要扩栈，memmove 只保留 [sp,0)。
 * 中间没有 safepoint，所以"先发布后写"不会让 GC 看到未初始化的格子。
 *
 * RT_LINK_SLOW 是 nov > 3 的稀有形状：溢出实参越过 slack，才需要现场扩栈。 */
#define RT_LINK(p, nb, kf, self, cb, nov)                                                          \
    do {                                                                                           \
        {                                                                                          \
            Val *h = rt_slotp((p), (nb) + RT_CBASE);                                               \
            h[0] = val_int((int64_t)(cb));                                                         \
            h[1] = val_int((int64_t)(kf));                                                         \
            h[2] = (self);                                                                         \
        }                                                                                          \
        RT_PUBLISH((p), (nb) + RT_SPILL + (nov));                                                  \
    } while (0)

#define RT_LINK_SLOW(p, nb, kf, self, cb, nov)                                                     \
    do {                                                                                           \
        rt_reserve((p), -((nb) + RT_SPILL + (nov) + 3));                                           \
        RT_LINK((p), (nb), (kf), (self), (cb), (nov));                                             \
    } while (0)

/* 尾调用：被调复用本帧（base），只需换 self + 发布溢出实参。不扩栈的理由同上
 * （nov ≤ 3 时 slack 兜住；RT_TAIL_ARGS_SLOW 处理其余）。 */
#define RT_TAIL_ARGS(p, b, nov)                                                                    \
    do {                                                                                           \
        RT_PUBLISH((p), (b) + RT_SPILL + (nov));                                                   \
    } while (0)

#define RT_TAIL_ARGS_SLOW(p, b, nov)                                                               \
    do {                                                                                           \
        rt_reserve((p), -((b) + RT_SPILL + (nov) + 3));                                            \
        RT_TAIL_ARGS((p), (b), (nov));                                                             \
    } while (0)

/* 让出：恢复点 = 即将进入的那个 stage，所以实参必须落进它的恢复格
 * [nb+RT_SPILL+ov, +3)，run_proc 唤醒时按 rt_resume[pc] 从那里读回 C 实参。
 * 冷路径（*bg<=0 才进来），随便花；不需要 reserve：RT_LINK/RT_TAIL_ARGS 已经
 * 多要了这 3 格。 */
#define RT_YIELD_CALL(p, sid, nb, ov, x1, x2, x3)                                                  \
    do {                                                                                           \
        {                                                                                          \
            Val *r = rt_slotp((p), (nb) + RT_SPILL + (ov) + 2);                                    \
            r[0] = (x3);                                                                           \
            r[1] = (x2);                                                                           \
            r[2] = (x1);                                                                           \
        }                                                                                          \
        RT_PUBLISH((p), (nb) + RT_SPILL + (ov) + 3);                                               \
        RT_YIELD((p), (sid), (nb));                                                                \
    } while (0)
#endif /* TA_RT_H */
