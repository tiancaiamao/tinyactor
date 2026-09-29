// lispvm.c — 最小 Lisp 内核字节码解释器（独立于 tinyactor VM）。
//
// 值表示照抄 tinyactor NaN-boxing（ta.h / ta_inline.h）：tag 高 16 位、
// 载荷低 48 位，pair/closure 是 arena cell 下标（bump 分配，v1 无 GC），
// 顶层函数引用走 TAG_CLOS_ID（免堆分配、CALL 零解引用）。
//
// 一栈模型（见 README.md）：帧 = 栈上连续 slot（fn、args、let 槽），
// 无 locals 数组、无返回栈；每函数 maxd 编译期算好，运行期唯一栈检查
// 在 CALL/TCALL：base + callee.maxd ≤ stack_cap。
//
// 用法: lispvm file.bc [--trace N]   （--trace 打印前 N 条指令轨迹）
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 值表示（ta.h 原样） ---- */
typedef uint64_t Val;
#define TAG_INT 0xFFF1
#define TAG_NIL 0xFFF2
#define TAG_TRUE 0xFFF3
#define TAG_FALSE 0xFFF4
#define TAG_SYM 0xFFF5
#define TAG_PAIR 0xFFF6
#define TAG_CLOS 0xFFF8
#define TAG_CLOS_ID 0xFFFB

static inline uint16_t val_tag(Val v) { return (uint16_t)(v >> 48); }
static inline uint64_t val_payload(Val v) { return v & 0x0000FFFFFFFFFFFFULL; }
static inline int val_is_int(Val v) { return val_tag(v) == TAG_INT; }

static inline Val val_int(int64_t i) {
    union {
        int64_t s;
        uint64_t u;
    } u;
    u.s = i;
    return ((uint64_t)TAG_INT << 48) | (u.u & 0x0000FFFFFFFFFFFFULL);
}
static inline int64_t val_get_int(Val v) {
    union {
        uint64_t u;
        int64_t s;
    } u;
    u.u = v & 0x0000FFFFFFFFFFFFULL;
    if (u.u & 0x800000000000ULL)
        u.u |= 0xFFFF000000000000ULL; /* 48 位符号扩展 */
    return u.s;
}
static inline Val val_tagged(uint16_t tag, uint64_t payload) {
    return ((uint64_t)tag << 48) | (payload & 0x0000FFFFFFFFFFFFULL);
}

/* ---- opcode（与 compile.ta 严格一致） ---- */
enum {
    OP_CONST = 0,
    OP_LOAD,
    OP_STORE,
    OP_LOADF,
    OP_PUSH,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_LT,
    OP_LE,
    OP_GT,
    OP_GE,
    OP_EQ,
    OP_PAIRP,
    OP_SYMP,
    OP_CONS,
    OP_CAR,
    OP_CDR,
    OP_JIF,
    OP_JUMP,
    OP_CALL,
    OP_TCALL,
    OP_RET,
    OP_MAKE_CLOSURE,
    OP_GLOBAL,
    OP_COUNT
};

/* ---- arena：bump 分配，v1 无 GC ---- */
static Val *arena;
static uint64_t arena_top, arena_cap;

static void oom(void) {
    fprintf(stderr, "lispvm: out of memory\n");
    exit(1);
}

static void arena_init(void) {
    arena_cap = 1 << 20;
    arena = malloc(arena_cap * sizeof(Val));
    if (!arena)
        oom();
}
static uint64_t arena_alloc(uint64_t words) {
    if (arena_top + words > arena_cap) {
        while (arena_top + words > arena_cap)
            arena_cap *= 2;
        arena = realloc(arena, arena_cap * sizeof(Val));
        if (!arena)
            oom();
    }
    uint64_t at = arena_top;
    arena_top += words;
    return at;
}
/* pair：两 cell（car, cdr）；closure：两 cell 头（fnid, nfree）+ nfree */
static Val mk_pair(Val car_v, Val cdr_v) {
    uint64_t at = arena_alloc(2);
    arena[at] = car_v;
    arena[at + 1] = cdr_v;
    return val_tagged(TAG_PAIR, at);
}
static Val mk_clos(uint64_t fnid, uint64_t nfree, const Val *frees) {
    uint64_t at = arena_alloc(2 + nfree);
    arena[at] = (Val)fnid;
    arena[at + 1] = (Val)nfree;
    for (uint64_t i = 0; i < nfree; i++)
        arena[at + 2 + i] = frees[i];
    return val_tagged(TAG_CLOS, at);
}

/* ---- 多文件链接（2.2）：每个 .bc 一个编译单元，顺序加载后统一重定位。
 * prog = 第一个文件（fn 0 = entry），其余为库（如 prelude.bc）。
 * 三个基址：词流首尾相接（word_base）、函数/常量池累加（fn/const_base）。
 * 重定位：CONST/MAKE_CLOSURE 按单元基址平移；GLOBAL 按符号名跨单元解析。 */

typedef struct {
    long word_base, fn_base, const_base, nfns;
} Unit;

#define MAX_UNITS 16
static Unit units[MAX_UNITS];
static int nunits;

static long *W; /* 所有单元的整数词，首尾相接 */
static long nwords, wcap;
static long nfns, nconsts; /* 全局总数（跨单元累加） */
static long *fn_entry, *fn_args, *fn_maxd, *fn_codelen, *fn_nameidx;
static Val *consts_g;    /* 解包后的常量（全局池，下标已含各单元基址） */
static char **sym_names; /* 符号名 intern 表（跨单元按名去重） */
static long nsyms, syms_cap;

static _Noreturn void fatal(const char *msg) {
    fprintf(stderr, "lispvm: %s\n", msg);
    exit(1);
}

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

static long intern_sym(const char *s, long len) {
    for (long j = 0; j < nsyms; j++)
        if (strcmp(sym_names[j], s) == 0)
            return j;
    if (nsyms == syms_cap) {
        syms_cap = syms_cap ? syms_cap * 2 : 64;
        sym_names = realloc(sym_names, (size_t)syms_cap * sizeof(char *));
        if (!sym_names)
            oom();
    }
    char *copy = malloc((size_t)len + 1);
    if (!copy)
        oom();
    memcpy(copy, s, (size_t)len);
    copy[len] = 0;
    sym_names[nsyms] = copy;
    return nsyms++;
}

/* 常量编码：0 int val | 1 nil | 2 true | 3 false | 4 sym len bytes */
static Val parse_const(long *pp) {
    long kind = W[(*pp)++];
    switch (kind) {
    case 0:
        return val_int(W[(*pp)++]);
    case 1:
        return val_tagged(TAG_NIL, 0);
    case 2:
        return val_tagged(TAG_TRUE, 0);
    case 3:
        return val_tagged(TAG_FALSE, 0);
    case 4: {
        long len = W[(*pp)++];
        /* 字节按词存（每词一个字节值），逐词取低字节；intern 复制后释放 */
        char *s = malloc((size_t)len + 1);
        if (!s)
            oom();
        for (long j = 0; j < len; j++)
            s[j] = (char)W[(*pp)++];
        s[len] = 0;
        /* intern：同名符号必须同 id，否则 eq? 按位比较会假阴 */
        long id = intern_sym(s, len);
        free(s);
        return val_tagged(TAG_SYM, (uint64_t)id);
    }
    default:
        fatal("bad const kind");
    }
}

static void parse_unit(const char *path) {
    if (nunits == MAX_UNITS)
        fatal("too many units");
    long wb = nwords;
    load_words(path);
    if (nwords - wb < 2)
        fatal("truncated file");
    long p = wb;
    long u_nfns = W[p++], u_nconsts = W[p++];
    if (u_nfns < 1)
        fatal("no functions");

    Unit *u = &units[nunits++];
    u->word_base = wb;
    u->fn_base = nfns;
    u->const_base = nconsts;
    u->nfns = u_nfns;

    /* 函数节：5 词头（nameidx entry nargs maxd codelen）+ 代码 */
    fn_entry = realloc(fn_entry, (size_t)(nfns + u_nfns) * sizeof(long));
    fn_args = realloc(fn_args, (size_t)(nfns + u_nfns) * sizeof(long));
    fn_maxd = realloc(fn_maxd, (size_t)(nfns + u_nfns) * sizeof(long));
    fn_codelen = realloc(fn_codelen, (size_t)(nfns + u_nfns) * sizeof(long));
    fn_nameidx = realloc(fn_nameidx, (size_t)(nfns + u_nfns) * sizeof(long));
    if (!fn_entry || !fn_args || !fn_maxd || !fn_codelen || !fn_nameidx)
        oom();
    for (long i = 0; i < u_nfns; i++) {
        long k = nfns + i;
        fn_nameidx[k] = W[p++]; /* 常量池符号下标；-1 = 匿名（entry/lambda） */
        if (fn_nameidx[k] >= 0)
            fn_nameidx[k] += u->const_base;
        fn_entry[k] = W[p++] + wb; /* 单元局部词偏移 → 全局词偏移 */
        fn_args[k] = W[p++];
        fn_maxd[k] = W[p++];
        fn_codelen[k] = W[p++];
        p += fn_codelen[k];
    }
    nfns += u_nfns;

    /* 常量节：追加到全局池 */
    consts_g = realloc(consts_g, (size_t)(nconsts + u_nconsts) * sizeof(Val));
    if (!consts_g)
        oom();
    for (long k = 0; k < u_nconsts; k++)
        consts_g[nconsts + k] = parse_const(&p);
    nconsts += u_nconsts;
}

static long find_cfunc(const char *name);

/* ---- 链接：名字注册 + 操作数重定位 ----
 * GLOBAL 操作数在文件里是（单元局部）常量池符号下标；此处平移后按名字
 * 解析为全局 fnid 并原地回写。extern 声明（编译侧 Bind(name,-2)）不产生
 * 函数记录，纯靠其它单元的 def 补项；补不到 = undefined global。 */
static const signed char g_optlen[OP_COUNT] = {
    [OP_CONST] = 2,        [OP_LOAD] = 2,  [OP_STORE] = 2, [OP_LOADF] = 2, [OP_PUSH] = 1,
    [OP_ADD] = 1,          [OP_SUB] = 1,   [OP_MUL] = 1,   [OP_DIV] = 1,   [OP_MOD] = 1,
    [OP_LT] = 1,           [OP_LE] = 1,    [OP_GT] = 1,    [OP_GE] = 1,    [OP_EQ] = 1,
    [OP_PAIRP] = 1,        [OP_SYMP] = 1,  [OP_CONS] = 1,  [OP_CAR] = 1,   [OP_CDR] = 1,
    [OP_JIF] = 3,          [OP_JUMP] = 2,  [OP_CALL] = 2,  [OP_TCALL] = 2, [OP_RET] = 1,
    [OP_MAKE_CLOSURE] = 3, [OP_GLOBAL] = 2};

static void link_units(void) {
    long *fn_of_sym = malloc((size_t)(nsyms > 0 ? nsyms : 1) * sizeof(long));
    if (!fn_of_sym)
        oom();
    for (long s = 0; s < nsyms; s++)
        fn_of_sym[s] = -1;
    for (long i = 0; i < nfns; i++) {
        if (fn_nameidx[i] < 0)
            continue;
        if (fn_nameidx[i] >= nconsts || val_tag(consts_g[fn_nameidx[i]]) != TAG_SYM)
            fatal("bad fn name const");
        long sid = (long)val_payload(consts_g[fn_nameidx[i]]);
        if (fn_of_sym[sid] >= 0) {
            fprintf(stderr, "lispvm: duplicate global '%s'\n", sym_names[sid]);
            exit(1);
        }
        fn_of_sym[sid] = i;
    }
    for (int ui = 0; ui < nunits; ui++) {
        Unit *u = &units[ui];
        for (long i = 0; i < u->nfns; i++) {
            long p = fn_entry[u->fn_base + i];
            long end = p + fn_codelen[u->fn_base + i];
            while (p < end) {
                int op = (int)W[p];
                if (op < 0 || op >= OP_COUNT || g_optlen[op] <= 0)
                    fatal("bad opcode");
                switch (op) {
                case OP_CONST:
                    W[p + 1] += u->const_base;
                    break;
                case OP_MAKE_CLOSURE:
                    W[p + 1] += u->fn_base;
                    break;
                case OP_GLOBAL: {
                    long o = W[p + 1] + u->const_base;
                    if (o < 0 || o >= nconsts || val_tag(consts_g[o]) != TAG_SYM)
                        fatal("GLOBAL: not a symbol const");
                    long sid = (long)val_payload(consts_g[o]);
                    long fid = fn_of_sym[sid];
                    if (fid < 0) {
                        long cidx = find_cfunc(sym_names[sid]);
                        if (cidx < 0) {
                            fprintf(stderr, "lispvm: undefined global '%s'\n", sym_names[sid]);
                            exit(1);
                        }
                        fid = nfns + cidx;
                    }
                    W[p + 1] = fid;
                    break;
                }
                }
                p += g_optlen[op];
            }
        }
    }
    free(fn_of_sym);
}

/* ---- host functions (Phase 4.1) ---- */
static void print_val(Val v);
typedef Val (*CFunc)(Val *args, long nargs);
typedef struct {
    const char *name;
    long nargs;
    CFunc fn;
} CFuncRec;

static Val cfunc_print(Val *args, long nargs) {
    (void)nargs;
    print_val(args[0]);
    printf("\n");
    return args[0];
}

static const CFuncRec cfuncs[] = {{"print", 1, cfunc_print}};
#define NCFUNCS ((long)(sizeof(cfuncs) / sizeof(cfuncs[0])))

static long find_cfunc(const char *name) {
    for (long i = 0; i < NCFUNCS; i++)
        if (strcmp(cfuncs[i].name, name) == 0)
            return i;
    return -1;
}

static void print_list(Val v) { /* v 是 pair，括号已由调用方打印 */
    print_val(arena[val_payload(v)]);
    Val rest = arena[val_payload(v) + 1];
    for (;;) {
        if (val_tag(rest) == TAG_NIL)
            return;
        if (val_tag(rest) != TAG_PAIR) {
            printf(" . ");
            print_val(rest);
            return;
        }
        printf(" ");
        print_val(arena[val_payload(rest)]);
        rest = arena[val_payload(rest) + 1];
    }
}

static void print_val(Val v) {
    switch (val_tag(v)) {
    case TAG_INT:
        printf("%" PRId64, val_get_int(v));
        break;
    case TAG_NIL:
        printf("nil");
        break;
    case TAG_TRUE:
        printf("true");
        break;
    case TAG_FALSE:
        printf("false");
        break;
    case TAG_SYM: {
        uint64_t id = val_payload(v);
        if (id < (uint64_t)nconsts && sym_names[id])
            printf("%s", sym_names[id]);
        else
            printf("#<sym%" PRIu64 ">", id);
        break;
    }
    case TAG_PAIR:
        printf("(");
        print_list(v);
        printf(")");
        break;
    case TAG_CLOS:
        printf("#<clos>");
        break;
    case TAG_CLOS_ID:
        printf("#<fn%" PRIu64 ">", val_payload(v));
        break;
    default:
        printf("#<?%04x>", val_tag(v));
        break;
    }
}

/* ---- 解释器主循环 ---- */
static long trace_left = 0;

static inline int truthy(Val v) {
    return v != val_tagged(TAG_NIL, 0) && v != val_tagged(TAG_FALSE, 0);
}

static void run(void) {
    long stack_cap = 1 << 20;
    Val *stack = malloc((size_t)stack_cap * sizeof(Val));
    if (!stack)
        oom();
    long sp = 0, base = 0, depth = 0; /* depth: CALL/RET 配对，entry RET 即结束 */
    long cbase = fn_entry[0];         /* JIF/JUMP 目标 = fn 内相对偏移 + cbase */
    long *rstack = malloc((size_t)stack_cap * 3 * sizeof(long));
    long rsp = 0;
    if (!rstack)
        oom();
    Val acc = val_tagged(TAG_NIL, 0);
    long pc = fn_entry[0];

    static void *dispatch[OP_COUNT] = {
        [OP_CONST] = &&op_const,   [OP_LOAD] = &&op_load,
        [OP_STORE] = &&op_store,   [OP_LOADF] = &&op_loadf,
        [OP_PUSH] = &&op_push,     [OP_ADD] = &&op_add,
        [OP_SUB] = &&op_sub,       [OP_MUL] = &&op_mul,
        [OP_DIV] = &&op_div,       [OP_MOD] = &&op_mod,
        [OP_LT] = &&op_lt,         [OP_LE] = &&op_le,
        [OP_GT] = &&op_gt,         [OP_GE] = &&op_ge,
        [OP_EQ] = &&op_eq,         [OP_PAIRP] = &&op_pairp,
        [OP_SYMP] = &&op_symp,     [OP_CONS] = &&op_cons,
        [OP_CAR] = &&op_car,       [OP_CDR] = &&op_cdr,
        [OP_JIF] = &&op_jif,       [OP_JUMP] = &&op_jump,
        [OP_CALL] = &&op_call,     [OP_TCALL] = &&op_tcall,
        [OP_RET] = &&op_ret,       [OP_MAKE_CLOSURE] = &&op_make_closure,
        [OP_GLOBAL] = &&op_global,
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
    acc = consts_g[W[pc + 1]];
    pc += 2;
    NEXT();
op_load:
    TRACE;
    acc = stack[base + W[pc + 1]];
    pc += 2;
    NEXT();
op_store:
    TRACE;
    stack[base + W[pc + 1]] = acc;
    pc += 2;
    NEXT();
op_loadf: {
    TRACE;
    Val clo = stack[base];
    if (val_tag(clo) != TAG_CLOS)
        fatal("LOADF on non-closure");
    acc = arena[val_payload(clo) + 2 + (uint64_t)W[pc + 1]];
    pc += 2;
    NEXT();
}
op_push:
    TRACE;
    stack[sp++] = acc;
    pc += 1;
    NEXT();

/* 二元：l = pop，acc = l op acc；int 门禁 */
#define ARITH(oper)                                                                                \
    do {                                                                                           \
        TRACE;                                                                                     \
        Val l = stack[--sp];                                                                       \
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
    Val l = stack[--sp];
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
    Val l = stack[--sp];
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
        Val l = stack[--sp];                                                                       \
        if (!val_is_int(l) || !val_is_int(acc))                                                    \
            fatal("cmp on non-int");                                                               \
        int64_t a = val_get_int(l), b = val_get_int(acc);                                          \
        acc = (a oper b) ? val_tagged(TAG_TRUE, 0) : val_tagged(TAG_FALSE, 0);                     \
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
    Val e = stack[--sp];
    /* 原始相等：int/bool/nil 按值，pair/clos/sym 按身份 —— NaN-boxing 下
     * 一律就是位相等 */
    acc = (e == acc) ? val_tagged(TAG_TRUE, 0) : val_tagged(TAG_FALSE, 0);
    pc += 1;
    NEXT();

op_pairp:
    TRACE;
    acc = (val_tag(acc) == TAG_PAIR) ? val_tagged(TAG_TRUE, 0) : val_tagged(TAG_FALSE, 0);
    pc += 1;
    NEXT();
op_symp:
    TRACE;
    acc = (val_tag(acc) == TAG_SYM) ? val_tagged(TAG_TRUE, 0) : val_tagged(TAG_FALSE, 0);
    pc += 1;
    NEXT();

op_cons: {
    TRACE;
    Val cdr_v = acc;
    Val car_v = stack[--sp];
    acc = mk_pair(car_v, cdr_v);
    pc += 1;
    NEXT();
}
op_car:
    TRACE;
    if (val_tag(acc) != TAG_PAIR)
        fatal("car on non-pair");
    acc = arena[val_payload(acc)];
    pc += 1;
    NEXT();
op_cdr:
    TRACE;
    if (val_tag(acc) != TAG_PAIR)
        fatal("cdr on non-pair");
    acc = arena[val_payload(acc) + 1];
    pc += 1;
    NEXT();

op_jif:
    TRACE;
    pc = truthy(acc) ? cbase + W[pc + 1] : cbase + W[pc + 2];
    NEXT();
op_jump:
    TRACE;
    pc = cbase + W[pc + 1];
    NEXT();

/* CALL/TCALL n（n = 参数数+1）：flush acc（末参）→ base = sp-n；
 * 唯一的栈检查点：base + callee.maxd ≤ stack_cap */
#define CALL_COMMON(is_tail)                                                                       \
    do {                                                                                           \
        TRACE;                                                                                     \
        long n = W[pc + 1];                                                                        \
        stack[sp++] = acc;                                                                         \
        long nb = sp - n;                                                                          \
        Val fv = stack[nb];                                                                        \
        uint64_t fid;                                                                              \
        if (val_tag(fv) == TAG_CLOS_ID) {                                                          \
            fid = val_payload(fv);                                                                 \
        } else if (val_tag(fv) == TAG_CLOS) {                                                      \
            fid = arena[val_payload(fv)];                                                          \
        } else {                                                                                   \
            fatal("call on non-function");                                                         \
        }                                                                                          \
        if (fid >= (uint64_t)nfns) {                                                               \
            uint64_t ci = fid - (uint64_t)nfns;                                                    \
            if (ci >= (uint64_t)NCFUNCS)                                                           \
                fatal("call: bad cfunc id");                                                       \
            if (cfuncs[ci].nargs != n - 1)                                                         \
                fatal("arity mismatch");                                                           \
            acc = cfuncs[ci].fn(&stack[nb + 1], n - 1);                                            \
            if (is_tail) {                                                                         \
                if (depth == 0)                                                                    \
                    goto done;                                                                     \
                depth--;                                                                           \
                sp = base;                                                                         \
                cbase = rstack[--rsp];                                                             \
                base = rstack[--rsp];                                                              \
                pc = rstack[--rsp];                                                                \
            } else {                                                                               \
                sp = nb;                                                                           \
                pc += 2;                                                                           \
            }                                                                                      \
            goto *dispatch[W[pc]];                                                                 \
        }                                                                                          \
        if (fn_args[fid] != n - 1)                                                                 \
            fatal("arity mismatch");                                                               \
        if (nb + fn_maxd[fid] > stack_cap)                                                         \
            fatal("stack overflow");                                                               \
        if (is_tail) {                                                                             \
            for (long i = 0; i <= n; i++)                                                          \
                stack[base + i] = stack[nb + i];                                                   \
            nb = base;                                                                             \
            sp = nb + n + 1;                                                                       \
        } else {                                                                                   \
            rstack[rsp++] = pc + 2;                                                                \
            rstack[rsp++] = base;                                                                  \
            rstack[rsp++] = cbase;                                                                 \
            depth++;                                                                               \
        }                                                                                          \
        pc = fn_entry[fid];                                                                        \
        base = nb;                                                                                 \
        cbase = pc;                                                                                \
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
    sp = base;
    cbase = rstack[--rsp];
    base = rstack[--rsp];
    pc = rstack[--rsp];
    NEXT();

op_make_closure: {
    TRACE;
    long fnid = W[pc + 1], nfree = W[pc + 2];
    if (nfree == 0) {
        acc = val_tagged(TAG_CLOS_ID, (uint64_t)fnid);
    } else {
        stack[sp++] = acc; /* flush（最后一个自由值） */
        acc = mk_clos((uint64_t)fnid, (uint64_t)nfree, &stack[sp - nfree]);
        sp -= nfree;
    }
    pc += 3;
    NEXT();
}
op_global:
    TRACE;
    acc = val_tagged(TAG_CLOS_ID, (uint64_t)W[pc + 1]);
    pc += 2;
    NEXT();

done:
    print_val(acc);
    printf("\n");
    free(stack);
}

int main(int argc, char **argv) {
    const char *files[MAX_UNITS];
    int nfiles = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_left = atol(argv[i + 1]);
            i++;
        } else {
            files[nfiles++] = argv[i];
        }
    }
    if (nfiles < 1) {
        fprintf(stderr, "usage: %s prog.bc [lib.bc ...] [--trace N]\n", argv[0]);
        return 1;
    }
    arena_init();
    wcap = 1 << 12;
    W = malloc((size_t)wcap * sizeof(long));
    if (!W)
        oom();
    for (int i = 0; i < nfiles; i++)
        parse_unit(files[i]);
    link_units();
    run();
    return 0;
}