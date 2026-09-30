// lispvm.c — Lisp 内核字节码解释器，宿主是 tinyactor 运行时。
//
// 目标是接回 TA：值表示、堆、GC、符号表、打印器、C 模块全部用 TA 本体
// （ta.h / ta_inline.h / src/*.c），这里新的只有 opcode 集和 dispatch 主循环，
// .bc 由 compile.ta 生成。此前自建的 arena / sym_names / print_val /
// g_natives 都是分叉，已删除——分叉越活越贵，接回去时全部作废。
//
// 栈：解释器的求值栈就是 TA 的 Proc 栈（p->sp 区间）。TA 的 GC 只扫
// p->sp 界定的区间（src/gc.c:161,196），私有 C 数组里的活值会被回收
// ——这是实测结论（见 gc_root_test.c / gc_proc_stack_test.c），不是推测。
// 因此本文件的 sp 与 p->sp 逐点同步：sp 是解释器视角的深度（向上长），
// p->sp = -sp。所有栈读写经 LSTK，所有 sp 变更经 SP_SET。
//
// 一栈模型（见 README.md）：帧 = 栈上连续 slot（fn、args、let 槽），
// 无 locals 数组、无返回栈；每函数 maxd 编译期算好。栈容量由 TA 的
// proc_push 自动增长兜底（栈堆相撞时扩 arena），不再有 stack_cap 检查。
//
// 用法: lispvm file.bc [--trace N]   （--trace 打印前 N 条指令轨迹）
#include "ta.h"

#include <dlfcn.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 宿主：TA 运行时 ---- */
static VM *g_vm;
static Proc *g_proc; /* 运行期间 tls_current_proc == g_proc */

static _Noreturn void fatal(const char *msg) {
    fprintf(stderr, "lispvm: %s\n", msg);
    exit(1);
}
static _Noreturn void oom(void) {
    fprintf(stderr, "lispvm: out of memory\n");
    exit(1);
}

/* ---- opcode（与 compile.ta 严格一致） ---- */
enum {
    LOP_CONST = 0,
    LOP_LOAD,
    LOP_STORE,
    LOP_LOADF,
    LOP_PUSH,
    LOP_ADD,
    LOP_SUB,
    LOP_MUL,
    LOP_DIV,
    LOP_MOD,
    LOP_LT,
    LOP_LE,
    LOP_GT,
    LOP_GE,
    LOP_EQ,
    LOP_PAIRP,
    LOP_SYMP,
    LOP_CONS,
    LOP_CAR,
    LOP_CDR,
    LOP_JIF,
    LOP_JUMP,
    LOP_CALL,
    LOP_TCALL,
    LOP_RET,
    LOP_MAKE_CLOSURE,
    LOP_GLOBAL,
    LOP_RESERVE,
    LOP_COUNT
};

/* ---- 栈：解释器深度 sp（含底部常量区）与 p->sp 的映射 ----
 *
 * TA 栈向下长：p->sp = -sp。slot i（0 起底）固定位于
 *   mem + mem_size - (i + 1) * sizeof(Val)
 * 即 index -(i+1)：栈底在最浅处（index -1），后压的更深。地址只取决于 i，
 * 与 sp 无关 —— 帧内 base+k 寻址因此与原实现完全一致。sp 只决定多少 slot
 * 是活的：GC 扫 [p->sp, 0) = slot 0..sp-1，恰好是本解释器的全部活值。 */
#define SP_SET(v)                                                                                  \
    do {                                                                                           \
        sp = (v);                                                                                  \
        g_proc->sp = -(int)sp;                                                                     \
    } while (0)
#define SP_ADJ(d) SP_SET((sp) + (d))
#define LSTK(i) (*(Val *)(g_proc->mem + g_proc->mem_size - ((int)(i) + 1) * (int)sizeof(Val)))

/* TAG_NATIVE：可调用值，载荷 = vm->symbols 下标（global 名）。
 * CALL 期才按名解析 cfunc（TA 的语义），所以这里只携带名字，不携带函数。 */
#define TAG_NATIVE 0xFFC1

/* 载荷读写：布局是 ta.h:30 文档化的公开约定（高 16 tag / 低 48 payload）。
 * TA 的 val_payload48/box_tag_payload 未导出，而 TAG_CLOS_ID/TAG_NATIVE 的
 * 载荷就是普通整数，这里按公开约定读写，pair/string/closure 一律走
 * val_get_car 等正式访问器，不碰指针。 */
static inline uint64_t lpayload(Val v) { return v & 0x0000FFFFFFFFFFFFULL; }
static inline Val lbox(uint16_t tag, uint64_t payload) {
    return ((uint64_t)tag << 48) | (payload & 0x0000FFFFFFFFFFFFULL);
}

/* ---- 加载：单个 .bc 编译单元 ---- */

static long *W; /* 本单元的整数词流 */
static long nwords, wcap;
static long nfns, nconsts;
static long *fn_entry, *fn_args, *fn_maxd, *fn_codelen, *fn_nameidx;
static long nconsts_off; /* 常量区占用的栈 slot 数（帧基址 = 它） */

static void load_words(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        fatal("cannot open file");
    long x;
    while (fscanf(f, "%ld", &x) == 1) {
        if (nwords == wcap) {
            wcap *= 2;
            W = realloc(W, (size_t)wcap * sizeof(long));
            if (!W)
                oom();
        }
        W[nwords++] = x;
    }
    fclose(f);
}

/* 常量编码：0 int val | 1 nil | 2 true | 3 false | 4 sym | 5 string len bytes */
static Val parse_const(long *pp) {
    long kind = W[(*pp)++];
    switch (kind) {
    case 0:
        return val_int(W[(*pp)++]);
    case 1:
        return val_nil();
    case 2:
        return val_true();
    case 3:
        return val_false();
    case 4: {
        /* 符号 intern 用 vm_intern_symbol：同名同 id，eq? 按位比较才成立，
         * 打印也交给 TA 的 print_val（走 vm->symbols）。 */
        long len = W[(*pp)++];
        char *s = malloc((size_t)len + 1);
        if (!s)
            oom();
        for (long j = 0; j < len; j++)
            s[j] = (char)W[(*pp)++];
        s[len] = 0;
        int id = vm_intern_symbol(g_vm, s);
        free(s);
        return val_symbol((uint32_t)id);
    }
    case 5: {
        long len = W[(*pp)++];
        char *s = malloc((size_t)len + 1);
        if (!s)
            oom();
        for (long j = 0; j < len; j++)
            s[j] = (char)W[(*pp)++];
        s[len] = 0;
        Val v = val_string(g_proc, s, (int)len);
        free(s);
        return v;
    }
    default:
        fatal("bad const kind");
    }
}

static void parse_unit(const char *path) {
    load_words(path);
    if (nwords < 2)
        fatal("truncated file");
    long p = 0;
    nfns = W[p++];
    nconsts = W[p++];
    if (nfns < 1)
        fatal("no functions");

    fn_entry = malloc((size_t)nfns * sizeof(long));
    fn_args = malloc((size_t)nfns * sizeof(long));
    fn_maxd = malloc((size_t)nfns * sizeof(long));
    fn_codelen = malloc((size_t)nfns * sizeof(long));
    fn_nameidx = malloc((size_t)nfns * sizeof(long));
    if (!fn_entry || !fn_args || !fn_maxd || !fn_codelen || !fn_nameidx)
        oom();

    /* 函数节：5 词头（nameidx entry nargs maxd codelen）+ 代码 */
    for (long i = 0; i < nfns; i++) {
        fn_nameidx[i] = W[p++]; /* 常量池符号下标；-1 = 匿名（entry/lambda） */
        fn_entry[i] = W[p++];
        fn_args[i] = W[p++];
        fn_maxd[i] = W[p++];
        fn_codelen[i] = W[p++];
        p += fn_codelen[i];
    }

    /* 常量节：常量常驻栈底（slot 0..nconsts-1），天然是 GC 根。
     * 先用 nil 占位（proc_push 负责扩容），再逐个覆写。 */
    for (long k = 0; k < nconsts; k++)
        proc_push(g_proc, val_nil());
    nconsts_off = nconsts;
    for (long k = 0; k < nconsts; k++) {
        Val v = parse_const(&p);
        *(Val *)(g_proc->mem + g_proc->mem_size - ((int)k + 1) * (int)sizeof(Val)) = v;
    }
}

/* ---- 单单元链接：GLOBAL 操作数（常量池符号下标）→ 本文件 fn_id，
 * 非本文件的名字改写成 -(vm 符号 id + 1)（负操作数 = 调用期按名解析的
 * cfunc）。这里只做"名字是不是本单元定义"的判定，不查任何函数表——
 * cfunc 解析在 CALL 期做，与 TA 一致。 */
static const signed char g_optlen[LOP_COUNT] = {
    [LOP_CONST] = 2,        [LOP_LOAD] = 2,   [LOP_STORE] = 2,  [LOP_LOADF] = 2, [LOP_PUSH] = 1,
    [LOP_ADD] = 1,          [LOP_SUB] = 1,    [LOP_MUL] = 1,    [LOP_DIV] = 1,   [LOP_MOD] = 1,
    [LOP_LT] = 1,           [LOP_LE] = 1,     [LOP_GT] = 1,     [LOP_GE] = 1,    [LOP_EQ] = 1,
    [LOP_PAIRP] = 1,        [LOP_SYMP] = 1,   [LOP_CONS] = 1,   [LOP_CAR] = 1,   [LOP_CDR] = 1,
    [LOP_JIF] = 3,          [LOP_JUMP] = 2,   [LOP_CALL] = 2,   [LOP_TCALL] = 2, [LOP_RET] = 1,
    [LOP_MAKE_CLOSURE] = 3, [LOP_GLOBAL] = 2, [LOP_RESERVE] = 2};

/* LSTKC：常量区 slot（link 期用，sp 恒为 nconsts_off） */
#define LSTKC(i) (*(Val *)(g_proc->mem + g_proc->mem_size - ((int)(i) + 1) * (int)sizeof(Val)))

static void link_unit(void) {
    long *fn_of_sym = malloc((size_t)(nconsts > 0 ? nconsts : 1) * sizeof(long));
    if (!fn_of_sym)
        oom();
    for (long s = 0; s < nconsts; s++)
        fn_of_sym[s] = -1;
    for (long i = 0; i < nfns; i++) {
        if (fn_nameidx[i] < 0)
            continue;
        if (fn_nameidx[i] >= nconsts || val_tag(LSTKC(fn_nameidx[i])) != TAG_SYM)
            fatal("bad fn name const");
        long sid = fn_nameidx[i];
        if (fn_of_sym[sid] >= 0)
            fatal("duplicate global");
        fn_of_sym[sid] = i;
    }
    for (long i = 0; i < nfns; i++) {
        long p = fn_entry[i];
        long end = p + fn_codelen[i];
        while (p < end) {
            int op = (int)W[p];
            if (op < 0 || op >= LOP_COUNT || g_optlen[op] <= 0)
                fatal("bad opcode");
            if (op == LOP_GLOBAL) {
                long o = W[p + 1];
                if (o < 0 || o >= nconsts || val_tag(LSTKC(o)) != TAG_SYM)
                    fatal("GLOBAL: not a symbol const");
                long fid = fn_of_sym[o];
                if (fid < 0) {
                    /* 非本单元定义：存 vm 符号 id，CALL 期按名找 cfunc */
                    W[p + 1] = -(long)(int)val_get_symbol(LSTKC(o)) - 1;
                } else {
                    W[p + 1] = fid;
                }
            }
            p += g_optlen[op];
        }
    }
    free(fn_of_sym);
}

/* ---- Erlang 式自动加载（照抄 src/vm.c CASE_LOP_CCALL_NAME 的 miss 路径）：
 * 名字带点 → dlopen lib/<mod>.<ext> → vm_load_self → 重试。找不到返回 -1。 */
static int find_cfunc_autoload(const char *name) {
    int cf = vm_find_cfunc(g_vm, name);
    if (cf >= 0)
        return cf;
    const char *dot = strchr(name, '.');
    if (!dot)
        return -1;
    int mod_len = (int)(dot - name);
#ifdef __APPLE__
    const char *ext = "dylib";
#else
    const char *ext = "so";
#endif
    char mod_path[256];
#ifdef TA_MOD_TAG
    int n = snprintf(mod_path, sizeof(mod_path), "lib/%.*s_%s.%s", mod_len, name,
                     TA_MOD_TAG_STR(TA_MOD_TAG), ext);
#else
    int n = snprintf(mod_path, sizeof(mod_path), "lib/%.*s.%s", mod_len, name, ext);
#endif
    if (n <= 0 || n >= (int)sizeof(mod_path))
        return -1;
    void *handle = dlopen(mod_path, RTLD_NOW | RTLD_GLOBAL);
    if (!handle)
        return -1;
    void (*reg)(VM *) = (void (*)(VM *))dlsym(handle, "vm_load_self");
    if (reg)
        reg(g_vm);
    return vm_find_cfunc(g_vm, name);
}

/* ---- 解释器主循环 ---- */
static long trace_left = 0;
/* -q：只跑不打印 entry 的值。 */
static int quiet = 0;

static void run(void) {
    long sp = 0, depth = 0; /* depth: CALL/RET 配对，entry RET 即结束 */
    /* 常量区已在 parse_unit 里入栈（p->sp 已同步），这里对齐本地 sp；
     * 帧基址从常量区之上起算。 */
    sp = nconsts_off;
    long base = sp;
    /* entry 帧的 slot（fn/args/RESERVE 区）也要在 p->sp 区间内：
     * 预压 maxd[0] 个 nil，RESERVE 只在其上再抬。 */
    for (long k = 0; k < fn_maxd[0]; k++)
        proc_push(g_proc, val_nil());
    sp += fn_maxd[0];
    g_proc->sp = -(int)sp;

    long cbase = fn_entry[0]; /* JIF/JUMP 目标 = fn 内相对偏移 + cbase */
    long *rstack = malloc((size_t)(1 << 16) * 3 * sizeof(long));
    long rsp = 0;
    if (!rstack)
        oom();
    Val acc = val_nil();
    long pc = fn_entry[0];

    static void *dispatch[LOP_COUNT] = {
        [LOP_CONST] = &&op_const,   [LOP_LOAD] = &&op_load,
        [LOP_STORE] = &&op_store,   [LOP_LOADF] = &&op_loadf,
        [LOP_PUSH] = &&op_push,     [LOP_ADD] = &&op_add,
        [LOP_SUB] = &&op_sub,       [LOP_MUL] = &&op_mul,
        [LOP_DIV] = &&op_div,       [LOP_MOD] = &&op_mod,
        [LOP_LT] = &&op_lt,         [LOP_LE] = &&op_le,
        [LOP_GT] = &&op_gt,         [LOP_GE] = &&op_ge,
        [LOP_EQ] = &&op_eq,         [LOP_PAIRP] = &&op_pairp,
        [LOP_SYMP] = &&op_symp,     [LOP_CONS] = &&op_cons,
        [LOP_CAR] = &&op_car,       [LOP_CDR] = &&op_cdr,
        [LOP_JIF] = &&op_jif,       [LOP_JUMP] = &&op_jump,
        [LOP_CALL] = &&op_call,     [LOP_TCALL] = &&op_tcall,
        [LOP_RET] = &&op_ret,       [LOP_MAKE_CLOSURE] = &&op_make_closure,
        [LOP_GLOBAL] = &&op_global, [LOP_RESERVE] = &&op_reserve,
    };

    goto *dispatch[W[pc]];

/* 轨迹：pc opcode sp/base acc */
#define TRACE                                                                                      \
    do {                                                                                           \
        if (trace_left > 0) {                                                                      \
            trace_left--;                                                                          \
            fprintf(stderr, "[%ld] op=%ld sp=%ld base=%ld acc=%016" PRIx64 "\n", pc, W[pc], sp,    \
                    base, acc);                                                                    \
        }                                                                                          \
    } while (0)

#define NEXT() goto *dispatch[W[pc]]

op_const:
    TRACE;
    acc = LSTK(W[pc + 1]); /* 常量常驻栈底 slot 0..nconsts-1 */
    pc += 2;
    NEXT();
op_load:
    TRACE;
    acc = LSTK(base + W[pc + 1]);
    pc += 2;
    NEXT();
op_store:
    TRACE;
    LSTK(base + W[pc + 1]) = acc;
    pc += 2;
    NEXT();
op_loadf: {
    TRACE;
    Val clo = LSTK(base);
    if (val_tag(clo) != TAG_CLOS)
        fatal("LOADF on non-closure");
    HeapClosure *hc = (HeapClosure *)(uintptr_t)lpayload(clo);
    acc = hc->free[W[pc + 1]];
    pc += 2;
    NEXT();
}
op_push:
    TRACE;
    proc_push(g_proc, acc);
    SP_ADJ(1);
    pc += 1;
    NEXT();

/* RESERVE n：函数体首指令，把 sp 抬过整个局部变量区。
 *
 * 帧布局：base+0 = fn 指针，base+1..base+nargs = 实参，base+nargs+1 起是
 * 局部变量，求值栈（push 区 / 被调帧）从 base+nargs+1+nlocals 往上长。
 * 编译器负责发这条指令 —— 值栈是一根共享栈，VM 不该猜调用方要几个槽。
 * 预压 nil 而不是只抬 sp：这些 slot 必须落在 p->sp 区间内 GC 才看得见。 */
op_reserve:
    TRACE;
    for (long k = 0; k < W[pc + 1]; k++)
        proc_push(g_proc, val_nil());
    SP_ADJ(W[pc + 1]);
    pc += 2;
    NEXT();

/* 二元：l = pop，acc = l op acc；int 门禁 */
#define ARITH(oper)                                                                                \
    do {                                                                                           \
        TRACE;                                                                                     \
        Val l = proc_pop(g_proc);                                                                  \
        SP_ADJ(-1);                                                                                \
        if (!val_is_int(l) || !val_is_int(acc))                                                    \
            fatal("arith on non-int");                                                             \
        int64_t a = val_get_int(l), b = val_get_int(acc);                                          \
        acc = val_int(a oper b);                                                                   \
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
    Val l = proc_pop(g_proc);
    SP_ADJ(-1);
    if (!val_is_int(l) || !val_is_int(acc))
        fatal("div on non-int");
    int64_t b = val_get_int(acc);
    if (b == 0)
        fatal("div by zero");
    acc = val_int(val_get_int(l) / b);
    pc += 1;
    NEXT();
}
op_mod: {
    TRACE;
    Val l = proc_pop(g_proc);
    SP_ADJ(-1);
    if (!val_is_int(l) || !val_is_int(acc))
        fatal("mod on non-int");
    int64_t b = val_get_int(acc);
    if (b == 0)
        fatal("mod by zero");
    acc = val_int(val_get_int(l) % b);
    pc += 1;
    NEXT();
}

/* 比较：acc = bool */
#define CMP(oper)                                                                                  \
    do {                                                                                           \
        TRACE;                                                                                     \
        Val l = proc_pop(g_proc);                                                                  \
        SP_ADJ(-1);                                                                                \
        if (!val_is_int(l) || !val_is_int(acc))                                                    \
            fatal("cmp on non-int");                                                               \
        int64_t a = val_get_int(l), b = val_get_int(acc);                                          \
        acc = (a oper b) ? val_true() : val_false();                                               \
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
        Val e = proc_pop(g_proc);
        SP_ADJ(-1);
        /* 原始相等：int/bool/nil 按值，pair/clos/sym 按身份 —— NaN-boxing 下
         * 一律就是位相等（pair 载荷是 TA 堆指针，身份语义随之成立） */
        acc = (e == acc) ? val_true() : val_false();
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
    Val cdr_v = acc;
    Val car_v = proc_pop(g_proc);
    SP_ADJ(-1);
    acc = val_pair(g_proc, car_v, cdr_v);
    pc += 1;
    NEXT();
}
op_car:
    TRACE;
    if (val_tag(acc) != TAG_PAIR)
        fatal("car on non-pair");
    acc = val_get_car(acc);
    pc += 1;
    NEXT();
op_cdr:
    TRACE;
    if (val_tag(acc) != TAG_PAIR)
        fatal("cdr on non-pair");
    acc = val_get_cdr(acc);
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
#define CALL_COMMON(is_tail)                                                                             \
    do {                                                                                                 \
        TRACE;                                                                                           \
        long n = W[pc + 1];                                                                              \
        proc_push(g_proc, acc);                                                                          \
        SP_ADJ(1);                                                                                       \
        long nb = sp - n;                                                                                \
        Val fv = LSTK(nb);                                                                               \
        uint64_t fid;                                                                                    \
        if (val_tag(fv) == TAG_CLOS_ID) {                                                                \
            fid = lpayload(fv);                                                                          \
        } else if (val_tag(fv) == TAG_CLOS) {                                                            \
            fid = (uint64_t)((HeapClosure *)(uintptr_t)lpayload(fv))->entry;                             \
        } else if (val_tag(fv) == TAG_NATIVE) {                                                          \
            long symidx = (long)lpayload(fv);                                                            \
            const char *name =                                                                           \
                (symidx >= 0 && symidx < g_vm->sym_count) ? g_vm->symbols[symidx] : NULL;                \
            int cf = name ? find_cfunc_autoload(name) : -1;                                              \
            if (cf < 0) {                                                                                \
                /* TA 语义：找不到 cfunc 就弹参压 nil，不 fatal */                           \
                SP_SET(is_tail ? base : nb);                                                             \
                acc = val_nil();                                                                         \
                pc += 2;                                                                                 \
                NEXT();                                                                                  \
            }                                                                                            \
            if (g_vm->cfuncs[cf].nargs != (int)(n - 1))                                                  \
                fatal("arity mismatch");                                                                 \
            /* 实参在 &LSTK(nb+1)（n-1 个连续 slot）。GC 门关上：TA 的 cfunc               \
             * 假定回调期间无回收（src/vm.c:1560），其 C 局部里的 Val 不在根集。*/ \
            Val *args = &LSTK(nb + 1);                                                                   \
            proc_gc_enter(g_proc);                                                                       \
            Val r = g_vm->cfuncs[cf].fn(g_vm, args, (int)(n - 1));                                       \
            proc_gc_leave(g_proc);                                                                       \
            SP_SET(is_tail ? base : nb);                                                                 \
            acc = r;                                                                                     \
            pc += 2;                                                                                     \
            NEXT();                                                                                      \
        } else {                                                                                         \
            fatal("call on non-function");                                                               \
        }                                                                                                \
        if (fn_args[fid] != n - 1)                                                                       \
            fatal("arity mismatch");                                                                     \
        if (is_tail) {                                                                                   \
            for (long i = 0; i <= n; i++)                                                                \
                LSTK(base + i) = LSTK(nb + i);                                                           \
            nb = base;                                                                                   \
            SP_SET(nb + n + 1);                                                                          \
        } else {                                                                                         \
            rstack[rsp++] = pc + 2;                                                                      \
            rstack[rsp++] = base;                                                                        \
            rstack[rsp++] = cbase;                                                                       \
            depth++;                                                                                     \
        }                                                                                                \
        pc = fn_entry[fid];                                                                              \
        base = nb;                                                                                       \
        cbase = pc;                                                                                      \
    } while (0)

op_call:
    CALL_COMMON(0);
    NEXT();
op_tcall:
    CALL_COMMON(1);
    NEXT();

op_ret:
    TRACE;
    if (depth == 0)
        goto done; /* entry 函数返回 = 程序结束 */
    depth--;
    SP_SET(base);
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
        proc_push(g_proc, acc); /* flush（最后一个自由值） */
        SP_ADJ(1);
        /* 自由值在 slot sp-nfree..sp-1，全在 p->sp 区间内。先分配（可能
         * 触发 GC，自由值因此被复制到新半区），再从栈上读——不能持指针。 */
        HeapClosure *hc = (HeapClosure *)proc_heap_alloc(g_proc, sizeof(HeapClosure) +
                                                                     (size_t)nfree * sizeof(Val));
        hc->hdr.type = HEAP_CLOS;
        hc->hdr.flags = 0;
        hc->entry = (int)fnid;
        hc->nfree = (int)nfree;
        for (long k = 0; k < nfree; k++)
            hc->free[k] = LSTK(sp - nfree + k);
        acc = lbox(TAG_CLOS, (uint64_t)(uintptr_t)hc);
        SP_ADJ(-nfree);
    }
    pc += 3;
    NEXT();
}
op_global:
    TRACE;
    {
        long o = W[pc + 1];
        /* 链接期负操作数 = 按名解析的 cfunc（载荷 = vm 符号 id）；
         * 非负 = 闭包 fnid */
        acc = o < 0 ? lbox(TAG_NATIVE, (uint64_t)(-(o + 1))) : lbox(TAG_CLOS_ID, (uint64_t)o);
    }
    pc += 2;
    NEXT();

done:
    if (!quiet) {
        print_val(g_vm, acc);
        printf("\n");
    }
    fflush(stdout);
}

static void host_init(void) {
    /* 不在 ta.h 里的模块注册入口（同 src/tavm.c:23-34） */
    extern void vm_register_file_module(VM * vm);
    extern void vm_register_os_module(VM * vm);
    extern void vm_register_buf_module(VM * vm);
    extern void vm_register_cov_module(VM * vm);
    extern void vm_register_str_module(VM * vm);
    extern void vm_register_num_modules(VM * vm);
    extern void vm_register_encoding_module(VM * vm);
    extern void vm_register_random_module(VM * vm);
    extern void vm_register_vm_module(VM * vm);
    extern void vm_register_tls_module(VM * vm);

    g_vm = vm_new();
    if (!g_vm)
        fatal("vm_new failed");
    /* 静态注册的 C 模块，与 src/tavm.c:104-116 同一份清单 */
    vm_register_net_module(g_vm);
    vm_register_tls_module(g_vm);
    vm_register_timer_module(g_vm);
    vm_register_file_module(g_vm);
    vm_register_os_module(g_vm);
    vm_register_buf_module(g_vm);
    vm_register_cov_module(g_vm);
    vm_register_str_module(g_vm);
    vm_register_num_modules(g_vm);
    vm_register_encoding_module(g_vm);
    vm_register_random_module(g_vm);
    vm_register_vm_module(g_vm);
    g_proc = proc_new(g_vm);
    if (!g_proc)
        fatal("proc_new failed");
    tls_current_proc = g_proc;
}

int main(int argc, char **argv) {
    const char *path = NULL;
    long trace = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace = atol(argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "-q") == 0) {
            quiet = 1;
        } else {
            path = argv[i];
        }
    }
    if (!path) {
        fprintf(stderr, "usage: %s prog.bc [-q] [--trace N]\n", argv[0]);
        return 1;
    }
    trace_left = trace;
    host_init();
    wcap = 1 << 12;
    W = malloc((size_t)wcap * sizeof(long));
    if (!W)
        oom();
    parse_unit(path);
    link_unit();
    run();
    return 0;
}
