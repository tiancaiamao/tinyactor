// tavm.c — Lisp 内核字节码解释器，宿主是 tinyactor 运行时。
//
// 目标是接回 TA：值表示、堆、GC、符号表、打印器、C 模块全部用 TA 本体
// （ta.h / ta_inline.h / src/*.c），这里新的只有 opcode 集和 dispatch 主循环，
// .tabc 由 compile.ta 生成。此前自建的 arena / sym_names / print_val /
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
// 用法: tavm file.tabc [--trace N]   （--trace 打印前 N 条指令轨迹）
#include "ta.h"
#include "ta_inline.h"

#include <dlfcn.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> /* clock_gettime: sched 空转的死锁采样超时 */
#include <unistd.h>

/* ---- 宿主：TA 运行时 ---- */
static VM *g_vm;
static Proc *g_proc; /* 运行期间 tls_current_proc == g_proc */

static _Noreturn void fatal(const char *msg) {
    fprintf(stderr, "tavm: %s\n", msg);
    exit(1);
}

/* 运行期类型/算术错误的进程隔离 die（vm.c 同款：'divzero / 'arithtype /
 * 'cartype / 'cdrtype）：只死当前 proc，monitor/link 传播、其余进程继续
 * 全部走共享 proc_die。仅限 run_proc 内使用 —— 中途 die 要先发布 pc/sp
 * （GC 与根扫描要一致状态），再清理解释器栈返回 R_DIED。 */
#define PROC_DIE_ISOLATED(reason)                                                                  \
    do {                                                                                           \
        int die_sym = vm_intern_symbol(g_vm, reason);                                              \
        st->pc = pc;                                                                               \
        st->rsp = rsp; /* 栈行走要的现场发布在 proc_die 前（lisp_walk_stack 用）*/   \
        st->depth = depth;                                                                         \
        SP_SET(sp);                                                                                \
        proc_die(g_vm, p, val_symbol((uint32_t)die_sym));                                          \
        free(rstack);                                                                              \
        st->rstack = NULL;                                                                         \
        return R_DIED;                                                                             \
    } while (0)

static _Noreturn void oom(void) {
    fprintf(stderr, "tavm: out of memory\n");
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
    LOP_BUILTIN,
    /* 融合指令：x + PUSH 一条发完（实参传递形态，compile.ta fuse pass
     * 生成，两边同步加）。操作数与原指令一致，语义 = 原指令 + proc_push。 */
    LOP_LOADP,
    LOP_CONSTP,
    LOP_GLOBP,
    LOP_COUNT
};

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

/* ---- 加载：单个 .tabc 编译单元 ---- */

static long *W; /* 本单元的整数词流 */
static long nwords, wcap;
static long nfns, nconsts;
static long *fn_entry, *fn_args, *fn_maxd, *fn_codelen, *fn_nameidx;
static long nconsts_off; /* 常量区占用的栈 slot 数（帧基址 = 它） */
static long g_const_pos; /* 常量节词流起点（push_image 重放用） */

static void push_image(Proc *proc);

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
    case 6: {
        /* 浮点字面量：kind 6 = len + UTF-8 文本（与字符串同布局），加载期
         * 一次 strtod。bootstrap 语言无 float 值，'float form 的文本端到端
         * 流到这里（同 codegen op_push_float 的约定）。strtod 失败给 0.0
         * —— 文本由自家 tokenizer 产生，只会是合法数字字面量。 */
        long len = W[(*pp)++];
        char *s = malloc((size_t)len + 1);
        if (!s)
            oom();
        for (long j = 0; j < len; j++)
            s[j] = (char)W[(*pp)++];
        s[len] = 0;
        double d = strtod(s, NULL);
        free(s);
        return val_float(d);
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

    nconsts_off = nconsts;
    g_const_pos = p;
    push_image(g_proc);
}

/* 常量节压入 proc 栈底（slot 0..nconsts-1，op_const = LSTK(k) 绝对寻址）。
 * 入口 proc 由 parse_unit 调用；spawn 的子 proc 由 run_proc 首跑调用 ——
 * parse_const 重新分配，常量值在各 proc 自己的堆里各有一份。 */
static void push_image(Proc *proc) {
    for (long k = 0; k < nconsts; k++)
        proc_push(proc, val_nil());
    long cp = g_const_pos;
    for (long k = 0; k < nconsts; k++) {
        Val v = parse_const(&cp);
        *(Val *)(proc->mem + proc->mem_size - ((int)k + 1) * (int)sizeof(Val)) = v;
    }
}

/* ---- 单单元链接：GLOBAL 操作数（常量池符号下标）→ 本文件 fn_id，
 * 非本文件的名字改写成 -(vm 符号 id + 1)（负操作数 = 调用期按名解析的
 * cfunc）。这里只做"名字是不是本单元定义"的判定，不查任何函数表——
 * cfunc 解析在 CALL 期做，与 TA 一致。 */
static const signed char g_optlen[LOP_COUNT] = {
    [LOP_CONST] = 2,        [LOP_LOAD] = 2,   [LOP_STORE] = 2,   [LOP_LOADF] = 2,   [LOP_PUSH] = 1,
    [LOP_ADD] = 1,          [LOP_SUB] = 1,    [LOP_MUL] = 1,     [LOP_DIV] = 1,     [LOP_MOD] = 1,
    [LOP_LT] = 1,           [LOP_LE] = 1,     [LOP_GT] = 1,      [LOP_GE] = 1,      [LOP_EQ] = 1,
    [LOP_PAIRP] = 1,        [LOP_SYMP] = 1,   [LOP_CONS] = 1,    [LOP_CAR] = 1,     [LOP_CDR] = 1,
    [LOP_JIF] = 3,          [LOP_JUMP] = 2,   [LOP_CALL] = 2,    [LOP_TCALL] = 2,   [LOP_RET] = 1,
    [LOP_MAKE_CLOSURE] = 3, [LOP_GLOBAL] = 2, [LOP_RESERVE] = 2, [LOP_BUILTIN] = 3, [LOP_LOADP] = 2,
    [LOP_CONSTP] = 2,       [LOP_GLOBP] = 2};

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
            if (op == LOP_GLOBAL || op == LOP_GLOBP) {
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

/* ---- Erlang 式自动加载：名字带点 → dlopen lib/<mod>.<ext> → vm_load_self
 * → 重试。找不到返回 -1，由调用点 fatal_unknown_cfunc 显式报错。 */
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

/* cfunc 解析不到（没注册、dlopen 不到模块、符号名拼错）：显式报错，不静默压 nil。
 * C 模块的成员表是可选的（不 import 就没有 .so）——编译期不拦调用头，但
 * 「名字在宿主里找不到任何实现」永远是错的程序：压 nil 会让错误以某个值错误的
 * 形式在很远的地方现形，而 nil 在 TA 里本身是「挂起/重试」的语义（issue #203）。
 * 报出缺的符号名 + 查找的模块，让「库没装」与「名字写错」当场可区分。 */
static _Noreturn void fatal_unknown_cfunc(const char *name) {
    if (name == NULL)
        fatal("native call with a bad symbol index");
    const char *dot = strchr(name, '.');
    if (dot == NULL)
        fprintf(stderr,
                "tavm: unknown C function '%s' (bare name: not registered by the host VM)\n", name);
    else
        fprintf(stderr, "tavm: unknown C function '%s' (module '%.*s')\n", name, (int)(dot - name),
                name);
    exit(1);
}

/* ---- 解释器主循环 ---- */
static long trace_left = 0;
/* -q：只跑不打印 entry 的值。 */
static int quiet = 0;

/* ---- actor 原语与调度 ----
 *
 * spawn/send/recv/self 编译为 LOP_BUILTIN：编译期固化整数 id，运行期
 * switch 派发（CALL 只服务 VM 自己的函数，cfunc 走 TAG_NATIVE 的 C
 * 模块协议 —— 分层：opcode / op_call / op_builtin / cfunc）。recv 可能
 * 阻塞，解释器状态（pc/base/cbase/sp/depth/返回栈）按 proc 存活期
 * 保存在 LState 侧表里；阻塞时 pc 不前进，唤醒后重执行本条指令。 */
typedef struct {
    long pc, base, cbase, sp, depth, rsp;
    long *rstack;
    long rstack_cap; /* malloc'd 容量（word 数）：深递归动态翻倍，tavm 无此上限 */
    long fnid;       /* 首个运行的 fn：0 = entry，spawn 填闭包 entry */
    int has_fn;      /* 子 proc：fn 槽（闭包）在常量区之上 */
    int started;     /* 0 = 从未运行（首跑做帧初始化） */
    Val fnval;       /* 子 proc 的闭包。安全性：spawn 到 run_proc 之间子 proc
                      * 不分配（无 GC），首跑 init 即压入栈成为根 */
    Val acc;         /* ccall yield 重入：末参在 acc（CALL 语义），阻塞时存这 */
    int has_acc;     /* recv 阻塞不读 acc（恒 0），ccall 重入置 1 */
    uint64_t prof_last; /* --profile：上一次采样时刻。放这不放 run_proc 局部——
                         * 跨热循环活跃的局部量会挤压解释器循环的寄存器分配
                         * （实测 +4%），与 pc/acc 同生命周期才对。 */
} LState;

static LState **g_lstate; /* 槽指针表：槽逐 pid 分配、地址恒定 */
static long g_lstate_cap;
static Proc *g_entry_proc;
static Val g_exit_val;
static int g_entry_done;
static int g_drain; /* entry 终止后的排空片计数（sched 收场批上限） */

#define R_DIED 0    /* proc 终止（entry RET = 程序结束；子 proc RET = 退休） */
#define R_BLOCKED 1 /* recv 阻塞：已登记 WAIT_RECV，等投递唤醒 */
/* 返回栈初始容量（word 数）。懒分配：不调用的 proc（如 worker 只 recv）
 * 零开销；曾预分配 192K words（1.5MB/proc），1M actor 空转出 1.5TB
 * VmSize + 4GB 首页 RSS。深递归由 CALL 处的翻倍 realloc 按需增长。 */
#define RSTACK_INIT_WORDS 4096

static LState *lstate_get(long pid) {
    if (pid >= g_lstate_cap) {
        /* 表几何扩容；不能按 procs_cap（默认 1M 槽 ≈ 109MB）首调全量
         * memset，那是 8ms 级启动税。槽本身单独 calloc：表扩容只搬指针，
         * run_proc / spawn 持有的 LState *st 全程有效（槽地址不随扩容变）。 */
        long nc = pid + 1;
        if (nc < g_lstate_cap * 2)
            nc = g_lstate_cap * 2;
        LState **nl = realloc(g_lstate, (size_t)nc * sizeof(LState *));
        if (!nl)
            oom();
        memset(nl + g_lstate_cap, 0, (size_t)(nc - g_lstate_cap) * sizeof(LState *));
        g_lstate = nl;
        g_lstate_cap = nc;
    }
    if (!g_lstate[pid]) {
        g_lstate[pid] = calloc(1, sizeof(LState));
        if (!g_lstate[pid])
            oom();
    }
    return g_lstate[pid];
}

/* ---- CRASH 帧名 / 栈行走（ta.h walk_stack 钩子）----
 *
 * 共享的 proc_die（scheduler.c）与 profiler 走 vm_walk_stack → 本钩子。
 * lisp 图的调用帧不在 p->mem（那是栈+堆），在 run_proc 的 LState 返回栈上：
 * 三元组 (ret_pc, base, cbase) 逐帧压入，rsp = 栈顶；叶帧 = 当前 pc，由
 * PROC_DIE_ISOLATED 在 proc_die 前发布。返回 fid = 图内 fn 下标，与
 * vm->fn_names（install_frame_hooks 按 fn_nameidx 填充）对齐；名字解析走
 * 共享 vm_fn_name 默认实现，profiler 同样受益。 */
static long lisp_fid_of_pc(long pc) {
    for (long i = 0; i < nfns; i++) {
        if (pc < fn_entry[i])
            return -1; /* 表按 emit 序递增，pc 还没进任何 fn */
        if (i + 1 >= nfns || pc < fn_entry[i + 1])
            return i;
    }
    return -1;
}

static int lisp_walk_stack(const VM *vm, const Proc *p, int *out, int max_depth) {
    (void)vm;
    if (p->pid < 0 || p->pid >= g_lstate_cap)
        return 0;
    LState *st = g_lstate[p->pid];
    if (!st || !st->started)
        return 0; /* 从未运行：报告只出 reason 行 */
    int depth = 0;
    long leaf = lisp_fid_of_pc(st->pc);
    if (leaf >= 0 && depth < max_depth)
        out[depth++] = (int)leaf;
    /* 返回栈惰性分配（首跑 rstack = NULL，首次 CALL 才 alloc）：顶层就崩的
     * proc 没有调用者帧，叶帧足够——与 tavm 单帧行为一致。 */
    if (st->rstack)
        for (long t = st->rsp - 3; t >= 0 && depth < max_depth; t -= 3) {
            long fid = lisp_fid_of_pc(st->rstack[t]);
            if (fid >= 0)
                out[depth++] = (int)fid;
        }
    return depth;
}

/* 装载后接通帧名：vm->fn_names[图内 fid]（fn_nameidx = -1 的匿名 fn 留
 * NULL → 打印 "?"）+ walk_stack 钩子。单图进程（tavm 只加载一个 .tabc），
 * 图内下标就是全局 fid。 */
static void install_frame_hooks(void) {
    g_vm->fn_names = calloc((size_t)(nfns > 0 ? nfns : 1), sizeof(char *));
    if (!g_vm->fn_names)
        oom();
    for (long i = 0; i < nfns; i++) {
        long ci = fn_nameidx[i];
        if (ci < 0 || ci >= nconsts)
            continue;
        Val v = LSTKC(ci);
        if (val_tag(v) != TAG_SYM)
            continue;
        long sid = val_get_symbol(v);
        if (sid < 0 || sid >= g_vm->sym_count)
            continue;
        g_vm->fn_names[i] = strdup(g_vm->symbols[sid]);
    }
    g_vm->fn_names_count = (int)nfns;
    g_vm->fn_names_cap = (int)nfns;
    /* self-time 排名（prof.c）按 [0, fn_count) 枚举 fid：lisp 的 fid 空间就是
     * 图内下标（单图进程），与 fn_names 同源。fn_table 不填（tavm 不用）。 */
    g_vm->fn_count = (int)nfns;
    g_vm->walk_stack = lisp_walk_stack;
}

/* ---- op_builtin 主体：actor 原语四条，放主循环外，别撑大派发热路径 ----
 *
 * 专用轻协议：前 n-1 参在栈顶（a1 = LSTK(sp-n+1) .. a(n-1) = LSTK(sp-1)），
 * 末参在 acc，无 fn 槽 —— 被调对象是编译期固化的整数 id（compile.ta B_*
 * 的镜像），switch 直接派发，零按名查找。结果写 *acc；tail 位置无需特殊
 * 处理（RET 会把 sp 收回 base）。零参/单参指令完全不动栈。
 * 返回非 0 = recv 阻塞：恢复点/栈已发布进 st，调用方直接 return R_BLOCKED。
 * LSTK 读 p 的栈：p == g_proc（run_proc 入口已绑定，本函数只从那里调）。 */
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
        uint32_t tpid = val_get_pid(LSTK(sp - 1));
        Proc *t = (tpid < (uint32_t)vm->procs_cap) ? vm->procs[tpid] : NULL;
        if (t && atomic_load(&t->state) != PROC_DEAD)
            mbox_deliver(vm, t, *acc);
        p->sp = -(long)(sp - 1); /* 弹栈上的 pid（等价 SP_SET(sp-1)） */
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
            proc_push(g_proc, m);
            SP_ADJ(1);
            proc_gc_drain(p);
            *acc = LSTK(sp - 1); /* 收集可能搬移，从栈重读 */
            SP_ADJ(-1);
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
            vm_wait_register(g_vm, p);
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
            vm_wait_register(g_vm, p);
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

static int run_proc(Proc *p, LState *st) {
    int resume = st->started;
    g_proc = p;
    tls_current_proc = p;
    long sp, depth, base, cbase, rsp, pc;
    long *rstack;
    Val acc;
    long budget = 1000; /* 本片预算：对齐 scheduler.c MAX_REDUCTIONS */
    /* 采样 profiler（--profile）：采样边界 = 预算边界（见 NEXT()），关断时
     * 热循环零指令。状态不占 run_proc 局部量：开关直读 g_vm，时刻在 st。 */
    if (g_vm->prof_on)
        st->prof_last = prof_now_ns();
    if (!resume) {
        /* 返回栈懒分配：首跑不 malloc，首次 CALL 压帧时经宏内翻倍 realloc
         * 从 NULL 起步（realloc(NULL,n)==malloc）。只 recv 不调用的 actor
         * （1M worker 场景）从此零 rstack 开销 —— 预分配曾让 1M proc 空转
         * 出 1.5TB VmSize。 */
        rstack = NULL;
        if (st->has_fn) {
            /* 子 proc：常量区自建（各堆一份），fn 槽（闭包）在其上。
             * 先整帧预留 + 铺满 fn/maxd 区，再建常量镜像：push_image 里
             * parse_const 的堆分配按**当时**的 p->sp 定堆的上界，若镜像
             * 建完再压 fn/maxd，堆 不知道这 41 个 slot 的存在，会长进
             * 帧区——proc_push 的碰撞处理没有 GC 兜底（只有
             * proc_stack_reserve / proc_heap_alloc 有），恰好塞满就
             * fatal（net-errno 的 spawn 复现：61 slot 镜像 + 40 slot
             * 帧 + 464B 堆 = 1536B 竞技场零余量）。镜像建的 61 个 nil
             * 会压到帧区上方成为垃圾，SP_SET 收回即可（GC 只扫 [sp,0)）。 */
            long total = nconsts + 1 + fn_maxd[st->fnid];
            /* fnval（闭包）此刻只被 C 变量持有，而接下来两步都会动堆：
             * ① proc_stack_reserve 帧放不下时走 gc_collect 搬移半区；
             * ② 常量镜像 parse_const 的堆分配可能触发 GC。GC 的根集
             * 只有栈——所以先把 fnval 压栈发布成根（reserve 的搬移
             * 原地更新栈槽），再从槽里读回转发后的地址继续用。
             * 不钉住的后果：捕获值全部读成 0（net-load 类）。 */
            proc_push(p, st->fnval);
            sp = 1;
            SP_SET(sp);
            proc_stack_reserve(p, -(int)(total + 1));
            Val fv = LSTK(0); /* GC 转发后的 fnval */
            sp = 0;
            SP_SET(sp);
            /* 整帧先铺 nil（堆分配由此看见最终栈深），再覆写常量区——
             * 与 push_image 同一覆写逻辑，但 nil 总数是整帧而非只常量区。
             * 帧顶再压临时根槽：parse_const 的分配仍可能触发 GC。 */
            for (long k = 0; k < total; k++)
                proc_push(p, val_nil());
            proc_push(p, fv);
            long cp = g_const_pos;
            for (long k = 0; k < nconsts; k++) {
                Val v = parse_const(&cp);
                *(Val *)(p->mem + p->mem_size - ((int)k + 1) * (int)sizeof(Val)) = v;
            }
            LSTK(nconsts) = LSTK(total); /* GC 转发后的 fnval */
            sp = total;                  /* 弹掉临时根槽 */
            SP_SET(sp);
        } else {
            /* entry：常量区已在 parse_unit 里入栈，这里对齐本地 sp */
            sp = nconsts;
            /* 帧的 slot（fn/args/RESERVE 区）也要在 p->sp 区间内：
             * 预压 maxd 个 nil，RESERVE 只在其上再抬。（子 proc 路径已
             * 在镜像前铺好 fn/maxd 区，不重复。） */
            for (long k = 0; k < fn_maxd[st->fnid]; k++)
                proc_push(p, val_nil());
            sp += fn_maxd[st->fnid];
        }
        base = nconsts;
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
    SP_SET(sp);

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
        if (trace_left > 0) {                                                                      \
            trace_left--;                                                                          \
            fprintf(stderr, "[%ld] op=%ld sp=%ld base=%ld acc=%016" PRIx64 "\n", pc, W[pc], sp,    \
                    base, acc);                                                                    \
        }                                                                                          \
    } while (0)

/* 栈安全全部走编译期数据：bc 里每个 fn 带 maxd（depthpass 对 Ins 树算的
 * 帧内峰值 = fn 槽 + 实参 + 局部 + push 区），CALL/TCALL 在解析出 fid 后
 * 按 base+maxd 一次预留 callee 整帧（proc_stack_reserve，能 grow 就 grow、
 * 需要时 gc）。帧内 PUSH/flush 落在已预留区间；堆侧分配由 proc_heap_alloc
 * 自带 gc+headroom 兜底。
 *
 * 但预留不是一劳永逸：堆侧深拷（如 monitor DOWN 投递进本 proc 堆）会把
 * heap_ptr 顶进预留但尚未使用的帧区 —— vm.c 靠每指令边界（TICK_FETCH）
 * 把栈余量拉回 TA_STACK_HEADROOM，tavm 此前没有这条边界，supervisor
 * 处理 DOWN 时 proc_push 直接撞 heap_ptr（arena 512B 起步、gc 未触发过）。
 * 这里补同款边界：只在余量不足时触发（开销同 vm.c 一条预测分支），acc
 * 可能持堆指针，先上栈成 GC 根再收集，取回即可。
 *
 * 同一边界顺带做 reduction 预算（vm.c TICK_FETCH 同款，MAX_REDUCTIONS 对
 * 齐 scheduler.c）：单线程调度没有预算，一个持续可运行的 actor（比如
 * recv_after(0) 忙循环）会在一次 run_proc 里跑满安全网上限，饿死整个
 * runq —— timer-sleep-concurrency 的 busy actor 就是这么把 4 个 sleeper
 * 饿到超时的。预算耗尽 = 完整解释器态进 LState、重新入队、回 sched。 */
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
            if (g_vm->prof_on) {                                                                                      \
                /* 采样边界 = 预算边界（每 1000 条指令一次）：关断时这条在 1/1000 的          \
                 * 冷分支里再判一次，热路径不为采样多付一条指令（旧的每指令 r++ 曾实测 \
                 * +5%）。叶帧：上面的 state 保存已把 st->pc 发到采样时刻。 */                     \
                uint64_t now = prof_now_ns();                                                                         \
                prof_collect(g_vm, p, now - st->prof_last);                                                           \
                st->prof_last = now;                                                                                  \
            }                                                                                                         \
            SP_SET(sp);                                                                                               \
            runq_enqueue(g_vm, p->pid); /* state 仍 RUNNING，sched 直接重跑 */                                  \
            return R_BLOCKED;                                                                                         \
        }                                                                                                             \
        if (p->heap_ptr > TA_PROC_CHUNK0 &&                                                                           \
            p->mem_size - p->heap_ptr - sp * (int)sizeof(Val) < TA_STACK_HEADROOM) {                                  \
            proc_push(g_proc, acc);                                                                                   \
            SP_ADJ(1);                                                                                                \
            proc_stack_headroom(g_proc); /* safe point：栈即全部根集 */                                        \
            acc = LSTK(sp - 1);                                                                                       \
            SP_ADJ(-1);                                                                                               \
        }                                                                                                             \
        goto *dispatch[W[pc]];                                                                                        \
    } while (0)

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

/* 融合指令（compile.ta fuse pass 发出）：x + PUSH 一步到位。acc 不动
 * ——与 LOAD/CONST/GLOB 的原语义一致（它们也是纯 acc 写）。 */
op_loadp:
    TRACE;
    proc_push(g_proc, LSTK(base + W[pc + 1]));
    SP_ADJ(1);
    pc += 2;
    NEXT();
op_constp:
    TRACE;
    proc_push(g_proc, LSTK(W[pc + 1]));
    SP_ADJ(1);
    pc += 2;
    NEXT();
op_globp:
    TRACE;
    {
        long o = W[pc + 1];
        proc_push(g_proc,
                  o < 0 ? lbox(TAG_NATIVE, (uint64_t)(-(o + 1))) : lbox(TAG_CLOS_ID, (uint64_t)o));
    }
    SP_ADJ(1);
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
    proc_stack_reserve(g_proc, -(int)(sp + W[pc + 1]));
    for (long k = 0; k < W[pc + 1]; k++)
        proc_push(g_proc, val_nil());
    SP_ADJ(W[pc + 1]);
    pc += 2;
    NEXT();

/* 二元算术：镜像 src/vm.c OP_ADD/OP_SUB/OP_MUL —— int∧int 保 int，其余
 * 走 double 路径（val_to_double 把非数值降为 0.0，与 golden 参考一致）。 */
#define ARITH(oper)                                                                                \
    do {                                                                                           \
        TRACE;                                                                                     \
        Val l = proc_pop(g_proc);                                                                  \
        SP_ADJ(-1);                                                                                \
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
    Val l = proc_pop(g_proc);
    SP_ADJ(-1);
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
    Val l = proc_pop(g_proc);
    SP_ADJ(-1);
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
        Val l = proc_pop(g_proc);                                                                  \
        SP_ADJ(-1);                                                                                \
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
        Val e = proc_pop(g_proc);
        SP_ADJ(-1);
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
    proc_push(g_proc, acc);
    SP_ADJ(1);
    HeapPair *hp = (HeapPair *)proc_heap_alloc(g_proc, sizeof(HeapPair));
    hp->hdr.type = HEAP_PAIR;
    hp->hdr.flags = 0;
    hp->cdr = LSTK(sp - 1);
    hp->car = LSTK(sp - 2);
    SP_ADJ(-2);
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
#define CALL_COMMON(is_tail)                                                                                             \
    do {                                                                                                                 \
        TRACE;                                                                                                           \
        long n = W[pc + 1];                                                                                              \
        proc_push(g_proc, acc);                                                                                          \
        SP_ADJ(1);                                                                                                       \
        long nb = sp - n;                                                                                                \
        Val fv = LSTK(nb);                                                                                               \
        uint64_t fid;                                                                                                    \
        if (val_tag(fv) == TAG_CLOS_ID) {                                                                                \
            fid = lpayload(fv);                                                                                          \
        } else if (val_tag(fv) == TAG_CLOS) {                                                                            \
            fid = (uint64_t)((HeapClosure *)(uintptr_t)lpayload(fv))->entry;                                             \
        } else if (val_tag(fv) == TAG_NATIVE) {                                                                          \
            long symidx = (long)lpayload(fv);                                                                            \
            const char *name =                                                                                           \
                (symidx >= 0 && symidx < g_vm->sym_count) ? g_vm->symbols[symidx] : NULL;                                \
            int cf = name ? find_cfunc_autoload(name) : -1;                                                              \
            /* TA 语义：解析不到 cfunc 就是错的程序，显式报错（不压 nil） */                       \
            if (cf < 0)                                                                                                  \
                fatal_unknown_cfunc(name);                                                                               \
            /* nargs==-1=声明的变参（net.connect 可选 timeout），个数由 cfunc 自校验；宿主 VM         \
             * 运行期不查 arity，此处只对固定参保留保险 */                                              \
            if (g_vm->cfuncs[cf].nargs >= 0 && g_vm->cfuncs[cf].nargs != (int)(n - 1))                                   \
                fatal("arity mismatch");                                                                                 \
            /* 实参按下标逐个拷进 C 数组 —— 不能取 &LSTK(nb+1) 当基址：                            \
             * 栈向低地址增长，C 数组方向（地址递增）与 slot 序（索引递增、                    \
             * 地址递减）相反，args[1] 会读到 fn 槽（bug：str.concat 恒空串）。*/                     \
            Val cargs[64];                                                                                               \
            if (n - 1 > 64)                                                                                              \
                fatal("too many arguments");                                                                             \
            for (int ai = 0; ai < (int)(n - 1); ai++)                                                                    \
                cargs[ai] = LSTK(nb + 1 + ai);                                                                           \
            /* 照抄 TA VM 的 OP_CCALL_NAME 协议（src/vm.c:1570-1622）：关门 +                                  \
             * in_ccall 窗口 —— 回调经 proc_heap_alloc 的分配改走 chunk arena，                           \
             * 不触发 moving GC，C 局部里的 Val 全程有效；返回后 converge 把                            \
             * chunk 结果收敛进 heap，否则结果 Val 在下次 GC 后悬垂。*/                                 \
            proc_gc_enter(g_proc);                                                                                       \
            g_proc->in_ccall = 1;                                                                                        \
            g_proc->yield_requested = 0;                                                                                 \
            Val r = g_vm->cfuncs[cf].fn(g_vm, cargs, (int)(n - 1));                                                      \
            if (g_proc->yield_requested) {                                                                               \
                /* 三段非阻塞 cfunc（net.connect/read…）请求重入：宿主协议同款                       \
                 * （src/vm.c OP_CCALL_NAME）—— pc 回到本指令起点，撤回入口处                         \
                 * flush 的 acc（重执行时重压），完整解释器态进 LState，WAIT_IO                        \
                 * 登记，io poller 唤醒后重执行本条 CALL。 */                                                \
                g_proc->yield_requested = 0;                                                                             \
                g_proc->in_ccall = 0;                                                                                    \
                proc_chunk_reset(g_proc);                                                                                \
                proc_gc_leave(g_proc);                                                                                   \
                st->acc = acc;                                                                                           \
                st->has_acc = 1;                                                                                         \
                st->pc = pc;                                                                                             \
                st->base = base;                                                                                         \
                st->cbase = cbase;                                                                                       \
                st->depth = depth;                                                                                       \
                st->rsp = rsp;                                                                                           \
                SP_SET(nb + n - 1); /* 撤回入口 flush 的 acc：回到指令入口 sp，重执行时重压 */        \
                st->sp = sp;                                                                                             \
                atomic_store(&g_proc->state, PROC_WAIT_IO);                                                              \
                vm_wait_register(g_vm, g_proc);                                                                          \
                return R_BLOCKED;                                                                                        \
            }                                                                                                            \
            g_proc->in_ccall = 0;                                                                                        \
            proc_chunk_converge(g_proc, &r);                                                                             \
            SP_SET(is_tail ? base : nb); /* 实参已消费，先弹掉（gate 仍关，无收集风险）*/             \
            proc_push(g_proc, r); /* 结果入栈作为根：下面的 drain 收集只扫栈 */                          \
            SP_ADJ(1);                                                                                                   \
            proc_gc_reopen(                                                                                              \
                g_proc);        /* tavm OP_CCALL 同款：补偿被关门压下的 GC 请求，                          \
                                 * 否则纯 ccall 工作负载（json/str 解析）请求永不清、堆只增不收 */ \
            acc = LSTK(sp - 1); /* 收集可能搬移对象，从栈重读（不能直接用 r） */                     \
            SP_SET(is_tail ? base : nb); /* 弹掉临时的结果根槽 */                                               \
            pc += 2;                                                                                                     \
            NEXT();                                                                                                      \
        } else {                                                                                                         \
            fatal("call on non-function");                                                                               \
        }                                                                                                                \
        if (fn_args[fid] != n - 1)                                                                                       \
            fatal("arity mismatch");                                                                                     \
        /* callee 整帧一次预留（base+maxd，maxd 含 fn 槽+实参+局部+push 峰值）：                       \
         * SP 变化指令处的唯一栈检查。此后 callee 内 PUSH/RESERVE 全落在                               \
         * 预留区间，无需任何边界检查。reserve 可能 gc（堆对象移动），                           \
         * fid 已提取成 long 不受影响；acc 已 flush 在栈上，GC 原地 forward。 */                        \
        if (is_tail) {                                                                                                   \
            proc_stack_reserve(g_proc, -(int)(base + fn_maxd[fid] + 1));                                                 \
            /* 帧恰 n 个 slot（fn + n-1 参）：多拷/多发布一个 slot 会把陈旧                            \
             * 栈垃圾放进 GC 根集扫描范围 —— GC 把陈旧堆指针当根，从                            \
             * fromspace 已回收区读垃圾头（"gc: unknown heap type 0"）。*/                                    \
            for (long i = 0; i < n; i++)                                                                                 \
                LSTK(base + i) = LSTK(nb + i);                                                                           \
            nb = base;                                                                                                   \
            SP_SET(nb + n);                                                                                              \
        } else {                                                                                                         \
            proc_stack_reserve(g_proc, -(int)(nb + fn_maxd[fid] + 1));                                                   \
            if (rsp + 3 > st->rstack_cap) {                                                                              \
                /* 返回栈满：翻倍 realloc（未分配时起步 RSTACK_INIT_WORDS，\ */                           \
                /* realloc(NULL,n)==malloc，支撑懒分配）。指针经 st->rstack \ */                              \
                /* 持久化，阻塞重入的 resume 路径会重读，旧副本不会复活。 */                      \
                long rcap = st->rstack_cap ? st->rstack_cap * 2 : RSTACK_INIT_WORDS;                                     \
                long *nrs = realloc(st->rstack, (size_t)rcap * sizeof(long));                                            \
                if (!nrs)                                                                                                \
                    oom();                                                                                               \
                rstack = nrs;                                                                                            \
                st->rstack = nrs;                                                                                        \
                st->rstack_cap = rcap;                                                                                   \
            }                                                                                                            \
            rstack[rsp++] = pc + 2;                                                                                      \
            rstack[rsp++] = base;                                                                                        \
            rstack[rsp++] = cbase;                                                                                       \
            depth++;                                                                                                     \
        }                                                                                                                \
        pc = fn_entry[fid];                                                                                              \
        base = nb;                                                                                                       \
        cbase = pc;                                                                                                      \
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
    if (do_builtin(g_vm, p, st, &acc, W[pc + 1], W[pc + 2], pc, base, cbase, sp, depth, rsp))
        return R_BLOCKED;
    sp = -p->sp; /* do_builtin 可能动栈（send 弹参），局部 sp 从权威源恢复 */
    pc += 3;
    NEXT();

op_ret:
    TRACE;
    if (depth == 0) {
        /* entry 返回 = 程序结束；子 proc 返回 = proc 终止（进退休表） */
        if (p == g_entry_proc) {
            g_exit_val = acc;
            g_entry_done = 1;
        } else {
            SP_SET(sp);
            proc_die(g_vm, p, val_nil());
        }
        free(rstack);
        st->rstack = NULL;
        return R_DIED;
    }
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
}

/* 调度循环：单线程，runq 逐个跑到死/阻塞。entry 终止 = 程序结束；
 * runq 空而 entry 未终 = 还有 proc 卡在 recv，无人投递 = 死锁。
 * （timer/recv_after 接入后，超时唤醒会经 vm_wait_register 之外的
 * deadline 扫描进 runq，这里再放宽。） */
static void sched(void) {
    int deadlock_suspect = 0; /* 空表采样连续次数：单次可能是投递瞬态 */
    g_entry_proc = g_proc;
    LState *st0 = lstate_get(g_proc->pid);
    st0->fnid = 0;
    st0->has_fn = 0;
    /* 三段非阻塞 cfunc（net.connect/read…）靠 io poller 唤醒重入；
     * recv 投递与 WAIT_IO 就绪都经 runq_enqueue 重新入队。 */
    vm_poller_start(g_vm);
    runq_enqueue(g_vm, g_proc->pid);
    for (;;) {
        int pid = runq_trydequeue(g_vm);
        if (pid < 0) {
            if (g_entry_done)
                return;
            /* entry 异常死亡（proc_die 已从 procs[] 摘除）：VM 随 main 终止
             * （tavm 同款 —— main 崩溃不该报 deadlock），退出码看 main_crashed。 */
            if (g_vm->procs[g_entry_proc->pid] == NULL)
                return;
            /* runq 空：所有阻塞者都在 wait 表（recv / WAIT_IO），poller 或
             * 投递会 runq_enqueue 唤醒（enqueue 侧 signal rq_cond），所以
             * 阻塞等条件变量而不是 usleep 轮询 —— 轮询曾在 1M actor 阻塞
             * 场景烧满单核（每拍 O(n) 扫 wait 链表）。
             * 死锁判定：runq 空且 wait 表空才可疑（投递方"摘表→入队"两步
             * 间的微秒级瞬态也会落进这个采样），连续两次超时（200ms）都
             * 如此才报 —— 对齐旧版"连续 100 拍 × 1ms"的防误报窗口。 */
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec += 100 * 1000 * 1000;
            if (deadline.tv_nsec >= 1000 * 1000 * 1000) {
                deadline.tv_sec += 1;
                deadline.tv_nsec -= 1000 * 1000 * 1000;
            }
            pthread_mutex_lock(&g_vm->rq_lock);
            if (atomic_load(&g_vm->rq_count) == 0)
                pthread_cond_timedwait(&g_vm->rq_cond, &g_vm->rq_lock, &deadline);
            pthread_mutex_unlock(&g_vm->rq_lock);
            if (atomic_load(&g_vm->rq_count) == 0 && vm_wait_count(g_vm) == 0) {
                if (++deadlock_suspect >= 2)
                    fatal("deadlock: no runnable or waiting procs");
            } else {
                deadlock_suspect = 0;
            }
            continue;
        }
        deadlock_suspect = 0; /* 有活可跑：重新累计连续空表采样 */
        if (pid >= (int)g_vm->procs_cap || !g_vm->procs[pid])
            continue;
        run_proc(g_vm->procs[pid], lstate_get(pid));
        /* entry 终止不立刻 return（tavm 同款）：send 已投递、receiver 已
         * 入 runq 的消息要排空再收场，否则消息往返类测试连 PASS 都来不及
         * 打。但排空必须有界 —— preempt 的无限自旋 proc 会永远再入队，
         * 无界排空 = 永不收场。对齐 tavm 单 worker 的 batch>=64 批上限：
         * entry 终止后至多再跑 64 片，然后强制收场（仍在 recv 的后台
         * proc 随 VM 终止，与 tavm 的 main-死亡语义一致）。 */
        if (g_entry_done && ++g_drain >= 64)
            return;
    }
}

/* ---- 裸名谓词 cfunc：compile.ta bare_cfunc 白名单的宿主侧契约 ----
 * 白名单把这些名字编译成按名 ccall，宿主必须注册同名 cfunc，否则运行期
 * fatal_unknown_cfunc 显式报错（以前是静默压 nil，nil 再喂给 cdr 就是
 * infer_lambda 崩的根因）。语义对齐 ta_inline.h
 * 的 val_is_*；list? = nil 或 pair（lisp 内核的表表示）；map? 内核尚无
 * map 值，恒 false（有真实用户时再定语义）。 */
static Val cfunc_intp(VM *vm, Val *a, int n) {
    (void)vm;
    (void)n;
    return val_is_int(a[0]) ? val_true() : val_false();
}
static Val cfunc_stringp(VM *vm, Val *a, int n) {
    (void)vm;
    (void)n;
    return val_is_string(a[0]) ? val_true() : val_false();
}
static Val cfunc_bytesp(VM *vm, Val *a, int n) {
    (void)vm;
    (void)n;
    return val_is_bytes(a[0]) ? val_true() : val_false();
}
static Val cfunc_pidp(VM *vm, Val *a, int n) {
    (void)vm;
    (void)n;
    return val_is_pid(a[0]) ? val_true() : val_false();
}
static Val cfunc_floatp(VM *vm, Val *a, int n) {
    (void)vm;
    (void)n;
    return val_is_float(a[0]) ? val_true() : val_false();
}
static Val cfunc_boolp(VM *vm, Val *a, int n) {
    (void)vm;
    (void)n;
    uint64_t t = val_tag(a[0]);
    return (t == TAG_TRUE || t == TAG_FALSE) ? val_true() : val_false();
}
static Val cfunc_nilp(VM *vm, Val *a, int n) {
    (void)vm;
    (void)n;
    return val_is_nil(a[0]) ? val_true() : val_false();
}
static Val cfunc_listp(VM *vm, Val *a, int n) {
    (void)vm;
    (void)n;
    return (val_is_nil(a[0]) || val_tag(a[0]) == TAG_PAIR) ? val_true() : val_false();
}
static Val cfunc_mapp(VM *vm, Val *a, int n) {
    (void)vm;
    (void)n;
    return val_false();
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
    vm_register(g_vm, "int?", cfunc_intp, 1);
    vm_register(g_vm, "string?", cfunc_stringp, 1);
    vm_register(g_vm, "bytes?", cfunc_bytesp, 1);
    vm_register(g_vm, "pid?", cfunc_pidp, 1);
    vm_register(g_vm, "float?", cfunc_floatp, 1);
    vm_register(g_vm, "bool?", cfunc_boolp, 1);
    vm_register(g_vm, "nil?", cfunc_nilp, 1);
    vm_register(g_vm, "list?", cfunc_listp, 1);
    vm_register(g_vm, "map?", cfunc_mapp, 1);
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
    /* entry = main 进程：main_pid 供 scheduler 的崩溃上报/退出码判定
     * （scheduler.c：entry 异常死亡置 main_crashed，tavm 退出码 1）。 */
    g_vm->main_pid = g_proc->pid;
    /* entry 起步用 idle 档 512B，按需翻倍+gc 爬梯，与 tavm spawn 出的
     * proc 同机制（常量区在栈上，堆侧无须预留）。曾预留 4MB：gc 每轮
     * calloc/free 整块 arena，string-churn 实测 144 轮 ×0.24ms ≈ 36ms
     * （同题 tavm 0.85ms）——按需增长反而更便宜。 */
    proc_ensure_heap(g_proc);
}

/* ---- 重入复位 ----
 *
 * native 一进程只跑一次 run，下面这些静态量天生就是初值；wasm Playground
 * 在同一模块实例里 callMain 先编译、再跑产物（反复点 Run 还会更多次），
 * 不归零就串台：load_words 是 append 语义（nwords 不清 → 新词流写飞），
 * LState 槽按 pid 复用（新 entry 还是 pid 0 → 读上一轮的 pc/started），
 * fn 表 / 词流 / 上一轮 VM 全泄漏。只在重入时有净效果，首跑各 free(NULL)。 */
static void run_reset(void) {
    free(W);
    W = NULL;
    nwords = 0;
    wcap = 0;
    free(fn_entry);
    free(fn_args);
    free(fn_maxd);
    free(fn_codelen);
    free(fn_nameidx);
    fn_entry = NULL;
    fn_args = NULL;
    fn_maxd = NULL;
    fn_codelen = NULL;
    fn_nameidx = NULL;
    nfns = 0;
    nconsts = 0;
    nconsts_off = 0;
    g_const_pos = 0;
    for (long i = 0; i < g_lstate_cap; i++) {
        if (!g_lstate[i])
            continue;
        free(g_lstate[i]->rstack);
        free(g_lstate[i]);
    }
    free(g_lstate);
    g_lstate = NULL;
    g_lstate_cap = 0;
    g_entry_proc = NULL;
    g_exit_val = 0; /* 静态初值（首跑 bit-identical） */
    g_entry_done = 0;
    g_drain = 0;
    quiet = 0;
    if (g_vm) {
        /* 上一轮 main 收尾已 vm_poller_stop（wasm 降级模式线程根本没起），
         * 这里把整个 VM 还掉——不还则每重入漏一张 1M 槽进程表 + 符号表。
         * vm_free 不触碰 tls_current_proc（其用途全在 cfunc 运行期）。 */
        vm_free(g_vm);
        g_vm = NULL;
        g_proc = NULL;
        tls_current_proc = NULL;
    }
}

int main(int argc, char **argv) {
    run_reset(); /* 重入场景静态态归零（详见函数注释）；首跑是空操作 */
    const char *path = NULL;
    long trace = 0;
    const char *prof_out = NULL;
    int argi = 1; /* 第一个非 flag 参数 = .tabc 路径，其后全是目标程序参数 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace = atol(argv[i + 1]);
            i++;
            argi = i + 1;
        } else if (strcmp(argv[i], "-q") == 0) {
            quiet = 1;
            argi = i + 1;
        } else if (strncmp(argv[i], "--profile", 9) == 0) {
            /* tavm 同款 --profile[=base]：预算边界采样（每 1000 条指令），prof_finish 写
             * <base>.json（speedscope）+ <base>.folded（折叠栈）。 */
            const char *a = argv[i];
            if (a[9] == '=' && a[10] != '\0')
                prof_out = a + 10;
            else if (a[9] == '\0')
                prof_out = "profile";
            else {
                fprintf(stderr, "error: unknown option: %s\n", a);
                return 1;
            }
            argi = i + 1;
        } else {
            path = argv[i];
            argi = i;
            break;
        }
    }
    if (!path) {
        fprintf(stderr,
                "usage: %s prog.tabc [-q] [--trace N] [--profile[=base]] [target-args...]\n",
                argv[0]);
        return 1;
    }
    trace_left = trace;
    host_init();
    extern void vm_set_argv(int argc, char **argv);
    /* Set argv for TA code: g_argv[0] = .tabc 路径，get_arg(0) = 第一个
     * 目标参数（与 tavm.c:119 的语义对齐）。 */
    vm_set_argv(argc - argi, argv + argi);
    wcap = 1 << 12;
    W = malloc((size_t)wcap * sizeof(long));
    if (!W)
        oom();
    parse_unit(path);
    link_unit();
    install_frame_hooks();
    if (prof_out)
        prof_init(g_vm, prof_out);
    sched();
    vm_poller_stop(g_vm);
    /* PR #104 诊断：TA_DUMP_INTERNS=<path> 时 dump 全局 intern 表（每行
     * "idx name"，按 intern 序，非可打印字符转 ?）；未设置 → 零影响
     * （tavm.c 同款语义，随 tavm 移植到 tavm main）。 */
    const char *dump_path = getenv("TA_DUMP_INTERNS");
    if (dump_path && *dump_path) {
        FILE *df = fopen(dump_path, "w");
        if (df) {
            for (int i = 0; i < g_vm->sym_count; i++) {
                fprintf(df, "%d ", i);
                const char *s = g_vm->symbols[i];
                if (!s) {
                    fprintf(df, "(null)");
                } else {
                    for (const unsigned char *c = (const unsigned char *)s; *c; c++)
                        fprintf(df, "%c", (*c >= 32 && *c < 127) ? *c : '?');
                }
                fprintf(df, "\n");
            }
            fclose(df);
        }
    }
    if (prof_out)
        prof_finish(g_vm);
    if (!quiet) {
        print_val(g_vm, g_exit_val);
        printf("\n");
    }
    fflush(stdout);
    /* main 崩溃 → 非零退出（tavm 同款；main_crashed 由 proc_die 路径置位） */
    return atomic_load(&g_vm->main_crashed) ? 1 : 0;
}
