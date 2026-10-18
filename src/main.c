// main.c — 宿主入口：CLI、.tabc 装载（parse_unit/link_unit）、帧名钩子；
// 运行循环（vm_run/worker_loop）在 scheduler.c，解释器本体在 vm.c，三 TU
// 经“装载接缝”（vm.c 的 extern 块）与 run_proc 约定衔接。
//
// 目标是接回 TA：值表示、堆、GC、符号表、打印器、C 模块全部用 TA 本体
// （ta.h / ta_inline.h / src/*.c），lisp 这层新的只有 opcode 集和 dispatch
// 主循环（在 vm.c），.tabc 由 compile.ta 生成。此前自建的 arena / sym_names /
// print_val / g_natives 都是分叉，已删除——分叉越活越贵，接回去时全部作废。
//
// 栈：求值栈就是 TA 的 Proc 栈（p->sp 区间）。TA 的 GC 只扫
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
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- 宿主：TA 运行时 ---- */
static VM *g_vm;

static _Noreturn void oom(void) {
    fprintf(stderr, "tavm: out of memory\n");
    exit(1);
}

/* ---- 加载：单个 .tabc 编译单元 ----
 *
 * 图像态（词流 / fn 表 / 常量表）归 VM 所有（vm->words 等，vm_free 释放），
 * 装载器经 g_vm 直接写入；下面的静态量是装载期 scratch，装载完只剩
 * fn_codelen / fn_nameidx（link_unit / install_frame_hooks 还要读）。 */
static long nwords, wcap;
static long nfns, nconsts;
static long *fn_codelen, *fn_nameidx;
static long g_const_pos; /* 常量节词流起点（build_consts 解析用） */
/* 字符串常量 payload 旁表（按常量下标并排；非字符串为 NULL）。g_vm->g_consts
 * 存的是 tagged Val，裸指针对 LSan 不可见，需要这张真实指针表把 payload
 * 挂在全局根上（见 build_consts）。 */
static HeapString **g_const_strs;

static void build_consts(void);

static void load_words(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        fatal("cannot open file");
    long x;
    while (fscanf(f, "%ld", &x) == 1) {
        if (nwords == wcap) {
            wcap *= 2;
            g_vm->words = realloc(g_vm->words, (size_t)wcap * sizeof(long));
            if (!g_vm->words)
                oom();
        }
        g_vm->words[nwords++] = x;
    }
    fclose(f);
}

/* 常量编码：0 int val | 1 nil | 2 true | 3 false | 4 sym | 5 string len bytes */
static Val parse_const(long *pp) {
    long kind = g_vm->words[(*pp)++];
    switch (kind) {
    case 0:
        return val_int(g_vm->words[(*pp)++]);
    case 1:
        return val_nil();
    case 2:
        return val_true();
    case 3:
        return val_false();
    case 4: {
        /* 符号 intern 用 vm_intern_symbol：同名同 id，eq? 按位比较才成立，
         * 打印也交给 TA 的 print_val（走 vm->symbols）。 */
        long len = g_vm->words[(*pp)++];
        char *s = malloc((size_t)len + 1);
        if (!s)
            oom();
        for (long j = 0; j < len; j++)
            s[j] = (char)g_vm->words[(*pp)++];
        s[len] = 0;
        int id = vm_intern_symbol(g_vm, s);
        free(s);
        return val_symbol((uint32_t)id);
    }
    case 5: {
        /* #278：字符串常量 malloc 直建（不进 proc 堆 → 永不搬移）。
         * HeapString 布局与 val_string 完全一致；TA 字符串不可变
         * （str.* 全走读访问器），全局共享一份无写者风险。 */
        long len = g_vm->words[(*pp)++];
        HeapString *hs = (HeapString *)malloc(sizeof(HeapString) + (size_t)len + 1);
        if (!hs)
            oom();
        hs->hdr.type = HEAP_STRING;
        hs->hdr.flags = 0;
        hs->len = (int)len;
        for (long j = 0; j < len; j++)
            hs->data[j] = (char)g_vm->words[(*pp)++];
        hs->data[len] = '\0';
        return lbox(TAG_STRING, (uint64_t)(uintptr_t)hs);
    }
    case 6: {
        /* 浮点字面量：kind 6 = len + UTF-8 文本（与字符串同布局），加载期
         * 一次 strtod。bootstrap 语言无 float 值，'float form 的文本端到端
         * 流到这里（约定见 lib/bootstrap/bytecode.ta const_words 的 kind 6）。strtod 失败给 0.0
         * —— 文本由自家 tokenizer 产生，只会是合法数字字面量。 */
        long len = g_vm->words[(*pp)++];
        char *s = malloc((size_t)len + 1);
        if (!s)
            oom();
        for (long j = 0; j < len; j++)
            s[j] = (char)g_vm->words[(*pp)++];
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
    if (nwords < 3)
        fatal("truncated file");
    long p = 0;
    long ver = g_vm->words[p++];
    if (ver != TA_BC_VERSION) {
        fprintf(stderr,
                "tavm: image version mismatch: expected %d, got %ld —— 旧布局 .tabc 与现行 "
                "continuation 协议不兼容，重新编译（make bootstrap）\n",
                TA_BC_VERSION, ver);
        exit(1);
    }
    nfns = g_vm->words[p++];
    nconsts = g_vm->words[p++];
    g_vm->nconsts = nconsts; /* vm_free 释放常量表 payload 用 */
    if (nfns < 1)
        fatal("no functions");

    g_vm->fn_entry = malloc((size_t)nfns * sizeof(long));
    g_vm->fn_args = malloc((size_t)nfns * sizeof(long));
    g_vm->fn_maxd = malloc((size_t)nfns * sizeof(long));
    fn_codelen = malloc((size_t)nfns * sizeof(long));
    fn_nameidx = malloc((size_t)nfns * sizeof(long));
    if (!g_vm->fn_entry || !g_vm->fn_args || !g_vm->fn_maxd || !fn_codelen || !fn_nameidx)
        oom();

    /* 函数节：5 词头（nameidx entry nargs maxd codelen）+ 代码 */
    for (long i = 0; i < nfns; i++) {
        fn_nameidx[i] = g_vm->words[p++]; /* 常量池符号下标；-1 = 匿名（entry/lambda） */
        g_vm->fn_entry[i] = g_vm->words[p++];
        g_vm->fn_args[i] = g_vm->words[p++];
        g_vm->fn_maxd[i] = g_vm->words[p++];
        fn_codelen[i] = g_vm->words[p++];
        p += fn_codelen[i];
    }

    g_const_pos = p;
    build_consts();
}

/* 常量节 → 全局表 g_vm->g_consts（一次，#278）。除 string 外的 kind 都是
 * 立即数：int/nil/bool/float 直接装箱，sym 走 vm_intern_symbol 进程级
 * intern —— 没有任何值挂在 proc 堆上，栈帧不再需要常量区。 */
static void build_consts(void) {
    if (nconsts <= 0)
        return;
    g_vm->g_consts = (Val *)malloc((size_t)nconsts * sizeof(Val));
    if (!g_vm->g_consts)
        oom();
    g_const_strs = (HeapString **)calloc((size_t)nconsts, sizeof(HeapString *));
    if (!g_const_strs)
        oom();
    long cp = g_const_pos;
    for (long k = 0; k < nconsts; k++) {
        Val v = parse_const(&cp);
        g_vm->g_consts[k] = v;
        /* tagged Val（高 16 位 tag）LSan 扫不出裸指针 payload —— 旁表用
         * 真实指针把字符串常量挂在全局根上，否则退出时被误报成泄漏
         * （Linux ASan 下 exit 42，任何带字符串常量的程序在 kernfuzz
         * 里都判 crash/dump-fail）。 */
        if (val_tag(v) == TAG_STRING)
            g_const_strs[k] = (HeapString *)(uintptr_t)lpayload(v);
    }
}

/* ---- 单单元链接：GLOBAL 操作数（常量池符号下标）→ 本文件 fn_id，
 * 非本文件的名字装载期按名解析成宿主 cfunc 下标存 -(cf+1)（带点名先
 * dlopen 自动加载），CALL 期直接按下标调用。这里判定"名字是不是本单元
 * 定义"：是 → fn_id，否 → find_cfunc_autoload。 */
static const signed char g_optlen[LOP_COUNT] = {
    [LOP_CONST] = 2,
    [LOP_LOAD] = 2,
    [LOP_STORE] = 2,
    [LOP_LOADF] = 2,
    [LOP_PUSH] = 1,
    [LOP_ADD] = 1,
    [LOP_SUB] = 1,
    [LOP_MUL] = 1,
    [LOP_DIV] = 1,
    [LOP_MOD] = 1,
    [LOP_LT] = 1,
    [LOP_LE] = 1,
    [LOP_GT] = 1,
    [LOP_GE] = 1,
    [LOP_EQ] = 1,
    [LOP_PAIRP] = 1,
    [LOP_SYMP] = 1,
    [LOP_CONS] = 1,
    [LOP_CAR] = 1,
    [LOP_CDR] = 1,
    [LOP_JIF] = 2,
    [LOP_JUMP] = 2,
    [LOP_CALL] = 2,
    [LOP_TCALL] = 2,
    [LOP_RET] = 1,
    [LOP_MAKE_CLOSURE] = 3,
    [LOP_GLOBAL] = 2,
    [LOP_RESERVE] = 2,
    [LOP_BUILTIN] = 3,
    [LOP_LOADP] = 2,
    [LOP_CONSTP] = 2,
    [LOP_GLOBP] = 2,
    [LOP_JIF_LT] = 3,
    [LOP_JIF_LE] = 3,
    [LOP_JIF_GT] = 3,
    [LOP_JIF_GE] = 3,
    [LOP_JIF_EQ] = 3,
    /* 立即数形态：词宽同源指令 +1（JIF_* 多带一个立即数字），但替代的
     * 「CONST_I + 源指令」少一个词 */
    [LOP_CONST_I] = 2,
    [LOP_ADD_I] = 2,
    [LOP_SUB_I] = 2,
    [LOP_JIF_EQ_I] = 4};

/* 把 rel（形如 lib/<mod>.<ext>）解析成宿主可执行文件旁的同名路径写入 out。
 * 懒加载 dylib 与 tavm 同树发布，而 tinyactor run 的运行半程跑在调用方 CWD
 * （用户程序的相对路径语义必须留在那里），CWD 相对查找就会落空——兜底到宿主
 * 位置，程序才能在任意目录运行（CWD 仍优先，dylib-missing 测试的 stub 覆盖
 * 语义不变）。失败返回 0。 */
static int host_module_path(char *out, size_t cap, const char *rel) {
    char exe[512];
#ifdef __APPLE__
    uint32_t size = (uint32_t)sizeof(exe);
    if (_NSGetExecutablePath(exe, &size) != 0)
        return 0;
#else
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0)
        return 0;
    exe[n] = '\0';
#endif
    char *slash = strrchr(exe, '/');
    if (!slash)
        return 0;
    *slash = '\0';
    int m = snprintf(out, cap, "%s/%s", exe, rel);
    return m > 0 && m < (int)cap;
}

/* ---- 装载期 cfunc 解析（link_unit 专用）：名字带点 → dlopen lib/<mod>.<ext>
 * → vm_load_self → 重试。名字属于**静态注册的宿主模块**（冻结前缀：num/vm/…）
 * 而成员名找不到时返回 -1，由 link_unit 调 fatal_unknown_cfunc 报「名字写错」;
 * 其余情况（懒加载模块）进不去文件 / 缺 vm_load_self / 加载了但没这个函数
 * 就地报专项诊断并 exit(1)——三种失败模式可区分，不退化成通用报错（issue #281）。
 *
 * 判据是冻结前缀，不是「注册表里有没有这个前缀」：懒加载 dylib 也会注册，
 * 后者会让同一个 miss 的报错随装载顺序变（--cov 插桩镜像自己先调 cov.hit，
 * 就把 cov 注册掉了，cov.nosuchop 会从 "provides no (lib/cov.dylib)" 降级成
 * 通用报错）。已加载过的懒加载模块再 dlopen 一次是幂等的（同一个 handle），
 * 而这条路只在报错时走到，不会热。 */
static int find_cfunc_autoload(VM *vm, const char *name) {
    int cf = vm_find_cfunc(vm, name);
    if (cf >= 0)
        return cf;
    const char *dot = strchr(name, '.');
    if (!dot)
        return -1;
    int mod_len = (int)(dot - name);
    /* 静态注册的宿主模块没有 lib/<mod>.<ext> 可加载，也不该按「缺库」报 */
    if (vm_host_module_registered(vm, name, mod_len))
        return -1;
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
    (void)dlerror(); /* 清残留错误，让下面拿到的 dlerror() 只属于本次 dlopen */
    void *handle = dlopen(mod_path, RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        /* CWD 相对查找落空 → 宿主旁兜底（见 host_module_path）；错误信息仍
         * 报 CWD 相对路径——那是用户能直接对照/重建的路径。 */
        char host_path[512];
        if (host_module_path(host_path, sizeof(host_path), mod_path)) {
            (void)dlerror();
            handle = dlopen(host_path, RTLD_NOW | RTLD_GLOBAL);
        }
    }
    if (!handle) {
        const char *err = dlerror();
        fprintf(stderr, "tavm: cannot load C module '%.*s' from %s: %s\n", mod_len, name, mod_path,
                err ? err : "unknown dlopen error");
        fprintf(stderr, "tavm: build the module first (e.g. `make %s`) or check the module name\n",
                mod_path);
        exit(1);
    }
    void (*reg)(VM *) = (void (*)(VM *))dlsym(handle, "vm_load_self");
    if (!reg) {
        fprintf(stderr,
                "tavm: C module '%.*s': %s does not export vm_load_self — not a TinyActor "
                "module or built for the wrong ABI; rebuild it\n",
                mod_len, name, mod_path);
        exit(1);
    }
    reg(vm);
    cf = vm_find_cfunc(vm, name);
    if (cf < 0) {
        fprintf(stderr,
                "tavm: C module '%.*s' (%s) provides no '%s' — stale module build or "
                "misspelled function name; rebuild it\n",
                mod_len, name, mod_path, name);
        exit(1);
    }
    return cf;
}

/* cfunc 解析不到（link 期没注册、dlopen 不到模块、符号名拼错）：显式报错，
 * 不静默压 nil。C 模块的成员表是可选的（不 import 就没有 .so）——编译期不拦
 * 调用头，但「名字在宿主里找不到任何实现」永远是错的程序：压 nil 会让错误以
 * 某个值错误的形式在很远的地方现形，而 nil 在 TA 里本身是「挂起/重试」的语义
 * （issue #203）。报出缺的符号名 + 查找的模块，让「库没装」与「名字写错」当场
 * 可区分。 */
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

static void link_unit(void) {
    long *fn_of_sym = malloc((size_t)(nconsts > 0 ? nconsts : 1) * sizeof(long));
    if (!fn_of_sym)
        oom();
    for (long s = 0; s < nconsts; s++)
        fn_of_sym[s] = -1;
    for (long i = 0; i < nfns; i++) {
        if (fn_nameidx[i] < 0)
            continue;
        if (fn_nameidx[i] >= nconsts || val_tag(g_vm->g_consts[fn_nameidx[i]]) != TAG_SYM)
            fatal("bad fn name const");
        long sid = fn_nameidx[i];
        if (fn_of_sym[sid] >= 0)
            fatal("duplicate global");
        fn_of_sym[sid] = i;
    }
    for (long i = 0; i < nfns; i++) {
        long p = g_vm->fn_entry[i];
        long end = p + fn_codelen[i];
        while (p < end) {
            int op = (int)g_vm->words[p];
            if (op < 0 || op >= LOP_COUNT || g_optlen[op] <= 0)
                fatal("bad opcode");
            if (op == LOP_GLOBAL || op == LOP_GLOBP) {
                long o = g_vm->words[p + 1];
                if (o < 0 || o >= nconsts || val_tag(g_vm->g_consts[o]) != TAG_SYM)
                    fatal("GLOBAL: not a symbol const");
                long fid = fn_of_sym[o];
                if (fid < 0) {
                    /* 非本单元定义：装载期一次按名解析成 cfunc 下标（带点名
                     * 先 dlopen 自动加载），存 -(cf+1)；CALL 期直接调用 */
                    long sid = (long)(int)val_get_symbol(g_vm->g_consts[o]);
                    const char *name =
                        (sid >= 0 && sid < g_vm->sym_count) ? g_vm->symbols[sid] : NULL;
                    int cf = name ? find_cfunc_autoload(g_vm, name) : -1;
                    if (cf < 0)
                        fatal_unknown_cfunc(name);
                    g_vm->words[p + 1] = -(long)cf - 1;
                } else {
                    g_vm->words[p + 1] = fid;
                }
            }
            p += g_optlen[op];
        }
    }
    free(fn_of_sym);
}

/* ---- 宿主侧运行态：quiet / 帧名钩子 ---- */
/* -q：只跑不打印 entry 的值。 */
static int quiet = 0;

/* ---- CRASH 帧名 / 栈行走（ta.h walk_stack 钩子）----
 *
 * 共享的 proc_die（scheduler.c）与 profiler 走 vm_walk_stack → 本钩子。
 * lisp 图的调用帧就在 p->mem 的栈区：每帧 fn@base, args, cont 三 TAG_INT
 * 槽（ret_pc, caller_base, caller_cbase @ base+nargs+1..+3，显式
 * continuation 协议，见 docs/backend.md）。叶帧 = 当前 pc/base
 * （PROC_DIE_ISOLATED 在 proc_die 前发布）；caller 帧沿 cont 回溯。
 * 返回 fid = 图内 fn 下标，与 vm->fn_names（install_frame_hooks 按
 * fn_nameidx 填充）对齐；名字解析走共享 vm_fn_name 默认实现，profiler
 * 同样受益。 */
static long lisp_fid_of_pc(long pc) {
    for (long i = 0; i < nfns; i++) {
        if (pc < g_vm->fn_entry[i])
            return -1; /* 表按 emit 序递增，pc 还没进任何 fn */
        if (i + 1 >= nfns || pc < g_vm->fn_entry[i + 1])
            return i;
    }
    return -1;
}

/* 栈槽读（walker 专用，LSTK 的 const 版）：slot i ↔ 顶部第 i+1 个 Val */
static const Val *walk_slot(const Proc *p, long i) {
    return (const Val *)((const char *)p->mem + p->mem_size - (i + 1) * (int)sizeof(Val));
}

static int lisp_walk_stack(const VM *vm, const Proc *p, int *out, int max_depth) {
    (void)vm;
    if (p->sp >= 0)
        return 0; /* 建帧前（proc_frame_init 之前）：无帧可走，报告只出 reason 行 */
    int depth = 0;
    long pc = p->pc;
    long base = p->base;
    long live = -(long)p->sp; /* 栈行走只读已发布现场（PROC_DIE_ISOLATED/TICK） */
    while (depth < max_depth) {
        long fid = lisp_fid_of_pc(pc);
        if (fid < 0)
            break;
        out[depth++] = (int)fid;
        if (base == 0)
            break; /* entry 帧：无 caller —— 顶层就崩的 proc 叶帧足够 */
        /* 调用者帧：cont 三槽在本帧尾（base+nargs+1..+3） */
        long nargs = g_vm->fn_args[fid];
        if (base + nargs + 3 > live)
            break; /* 防御：cont 不在已发布栈深内（现场被截断） */
        pc = val_get_int(*walk_slot(p, base + nargs + 1));
        base = val_get_int(*walk_slot(p, base + nargs + 2));
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
        Val v = g_vm->g_consts[ci];
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

/* ---- 裸名谓词 cfunc：compile.ta bare_cfunc 白名单的宿主侧契约 ----
 * 白名单把这些名字编译成按名 ccall，宿主必须注册同名 cfunc，否则装载期
 * link_unit 的 fatal_unknown_cfunc 显式报错（以前是静默压 nil，nil 再喂给 cdr 就是
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

/* worker 池大小 = NWORKERS 环境变量，默认 1（不设 = 单线程内联，与历史行为
 * 逐字节同路径）。默认关是刻意的：多核是显式开的能力，不是默认行为 ——
 * 所有既有测试/基准的语义基准是单线程，池只加并行度、不改语义
 * （mt-revival.md §7 开放问题#2 的拍板）。垃圾值/1/0/负数一律回落默认，
 * 不报错；上限由池自己钳（scheduler.c 的 MAX_WORKERS），宿主不认识池的界。 */
static void setup_nworkers(VM *vm) {
    const char *env = getenv("NWORKERS");
    if (!env || !*env)
        return;
    int n = atoi(env);
    if (n > 1)
        vm->nworkers = n;
}

static Proc *host_init(void) {
    /* 不在 ta.h 里的模块注册入口（同 src/main.c:23-34）
     * file / os / cov 已下沉为 lazy dylib（首个 file.* / os.raw_* /
     * cov.* 引用时 find_cfunc_autoload 自动 dlopen），不再在此静态注册。 */
    extern void vm_register_num_modules(VM * vm);
    extern void vm_register_vm_module(VM * vm);

    g_vm = vm_new();
    if (!g_vm)
        fatal("vm_new failed");
    setup_nworkers(g_vm);
    vm_register(g_vm, "int?", cfunc_intp, 1);
    vm_register(g_vm, "string?", cfunc_stringp, 1);
    vm_register(g_vm, "bytes?", cfunc_bytesp, 1);
    vm_register(g_vm, "pid?", cfunc_pidp, 1);
    vm_register(g_vm, "float?", cfunc_floatp, 1);
    vm_register(g_vm, "bool?", cfunc_boolp, 1);
    vm_register(g_vm, "nil?", cfunc_nilp, 1);
    vm_register(g_vm, "list?", cfunc_listp, 1);
    vm_register(g_vm, "map?", cfunc_mapp, 1);
    /* 静态注册的 C 模块，与 src/main.c:104-116 同一份清单 */
    vm_register_num_modules(g_vm);
    vm_register_vm_module(g_vm);
    /* Registry above is host-static from here on: vm.is_builtin_module
     * (import 解析) must keep answering "no source layer" for exactly these
     * names even after a lazy dylib (str/file/os/sexp/buffer) dlopens. */
    vm_seal_host_modules(g_vm);
    Proc *entry = proc_new(g_vm);
    if (!entry)
        fatal("proc_new failed");
    tls_current_proc = entry;
    /* entry = main 进程：main_pid 供 scheduler 的崩溃上报/退出码判定
     * （scheduler.c：entry 异常死亡置 main_crashed，tavm 退出码 1）。 */
    g_vm->main_pid = entry->pid;
    /* entry 起步用 idle 档 512B，按需翻倍+gc 爬梯，与 tavm spawn 出的
     * proc 同机制（常量区在栈上，堆侧无须预留）。曾预留 4MB：gc 每轮
     * calloc/free 整块 arena，string-churn 实测 144 轮 ×0.24ms ≈ 36ms
     * （同题 tavm 0.85ms）——按需增长反而更便宜。 */
    proc_ensure_heap(entry);
    return entry;
}

/* ---- 重入复位 ----
 *
 * native 一进程只跑一次 run，下面这些静态量天生就是初值；wasm Playground
 * 在同一模块实例里 callMain 先编译、再跑产物（反复点 Run 还会更多次），
 * 不归零就串台：load_words 是 append 语义（nwords 不清 → 新词流写飞），
 * fn 表 / 词流 / 上一轮 VM 全泄漏。恢复态不在这里清：它已随 Proc
 * 归并（新 entry = 新 Proc = calloc 归零），只随 vm_free 一起还。
 * 只在重入时有净效果，首跑各 free(NULL)。 */
static void run_reset(void) {
    free(fn_codelen);
    free(fn_nameidx);
    fn_codelen = NULL;
    fn_nameidx = NULL;
    /* 旁表只是 payload 的真实指针视图（LSan 根，847c994），不拥有内存：
     * 先拆视图、再拆源——源（vm->g_consts 及字符串 payload）在下面
     * vm_free 里释放，视图必须先于它。漏还则下一轮 build_consts 的
     * calloc 覆写静态指针，旧表在重入场景被 LSan 判漏。 */
    free(g_const_strs);
    g_const_strs = NULL;
    /* load_words 是 append 语义（nwords 不清 → 新词流写飞）；
     * 词流/fn 表/常量表本体随下面的 vm_free 一起还。 */
    nwords = 0;
    wcap = 0;
    nfns = 0;
    nconsts = 0;
    g_const_pos = 0;
    quiet = 0;
    if (g_vm) {
        /* 上一轮 main 收尾已 vm_poller_stop（wasm 降级模式线程根本没起），
         * 这里把整个 VM 还掉——不还则每重入漏一张 1M 槽进程表 + 符号表。
         * vm_free 不触碰 tls_current_proc（其用途全在 cfunc 运行期）。 */
        vm_free(g_vm);
        g_vm = NULL;
        tls_current_proc = NULL;
    }
}

/* ---- 信号：优雅停机 ----
 *
 * SIGINT/SIGTERM → 置 VM 的 stop 标志 + 唤醒可能阻塞在 poll() 的 io poller，
 * 让长驻程序（如 lib/serve.ta，main 永不返回）干净退出并打印 exit_val。
 * 这是 493f678 删 src/tavm.c 时随旧宿主一起丢失的行为（#358）。
 *
 * handler 只做 async-signal-safe 的两件事：atomic_store + 往 wake pipe
 * write。刻意不调 vm_request_stop 的 pthread_cond_broadcast——信号可能打断
 * 正持 rq_lock 的线程，在 handler 里抢同一把锁会自死锁；worker 侧是
 * timedwait（scheduler.c，100ms 兜底），漏一次广播最多多等一拍。
 * g_vm 在 wasm 重入（run_reset 置空）窗口里为 NULL，必须防空。 */
static void on_signal(int sig) {
    (void)sig;
    VM *vm = g_vm;
    if (vm) {
        atomic_store(&vm->stop, 1);
        vm_wake_poller(vm);
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
    Proc *entry = host_init();
    /* 优雅停机（#358）：进程级信号处置，在 VM 就绪后装配；wasm 重入会
     * 反复经过这里，signal() 幂等。SIGPIPE 的忽略在 lib/net.c 的 vm_load_self
     * （懒加载单次入口），不在这里——它不是宿主启动路径的职责（#323/#357）。 */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    g_vm->trace_left = trace;
    extern void vm_set_argv(int argc, char **argv);
    /* Set argv for TA code: g_argv[0] = .tabc 路径，get_arg(0) = 第一个
     * 目标参数（与 main.c:119 的语义对齐）。 */
    vm_set_argv(argc - argi, argv + argi);
    wcap = 1 << 12;
    g_vm->words = malloc((size_t)wcap * sizeof(long));
    if (!g_vm->words)
        oom();
    parse_unit(path);
    link_unit();
    install_frame_hooks();
    if (prof_out)
        prof_init(g_vm, prof_out);
    vm_run(g_vm, entry);
    /* PR #104 诊断：TA_DUMP_INTERNS=<path> 时 dump 全局 intern 表（每行
     * "idx name"，按 intern 序，非可打印字符转 ?）；未设置 → 零影响。 */
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
        print_val(g_vm, g_vm->exit_val);
        printf("\n");
    }
    fflush(stdout);
    /* main 崩溃 → 非零退出（main_crashed 由 proc_die 路径置位） */
    return atomic_load(&g_vm->main_crashed) ? 1 : 0;
}
