/* host.c — 两个宿主（字节码 src/main.c、C 后端 src/rt.c）共用的初始化。
 *
 * 这里只有一件事是必须共用的：裸名谓词 cfunc 的清单。它是 compile.ta
 * `bare_cfunc` 白名单的宿主侧契约 —— 白名单里的名字编译成按名 ccall，
 * 宿主漏注册就炸在装载期（link_unit 的 fatal_unknown_cfunc）。表只有一份，
 * 两种后端才不可能对"float? 到底存不存在"产生分歧。
 */
#include "host.h"

#include <stdlib.h>
#include <string.h>

/* C 的 1/0 → TA 布尔值：九个入口的公共收尾。 */
static Val truth(int yes) { return yes ? val_true() : val_false(); }

/* 语义对齐 ta_inline.h 的 val_is_*。 */
static Val cfunc_intp(VM *vm, Val *a, int n) { return truth(val_is_int(a[0])); }
static Val cfunc_stringp(VM *vm, Val *a, int n) { return truth(val_is_string(a[0])); }
static Val cfunc_bytesp(VM *vm, Val *a, int n) { return truth(val_is_bytes(a[0])); }
static Val cfunc_pidp(VM *vm, Val *a, int n) { return truth(val_is_pid(a[0])); }
static Val cfunc_floatp(VM *vm, Val *a, int n) { return truth(val_is_float(a[0])); }
/* bool?: TAG_TRUE / TAG_FALSE 两个立即数标签，没有 val_is_* 可用。 */
static Val cfunc_boolp(VM *vm, Val *a, int n) {
    uint64_t t = val_tag(a[0]);
    return truth(t == TAG_TRUE || t == TAG_FALSE);
}
static Val cfunc_nilp(VM *vm, Val *a, int n) { return truth(val_is_nil(a[0])); }
/* list? = nil 或 pair（lisp 内核的表表示）。 */
static Val cfunc_listp(VM *vm, Val *a, int n) {
    return truth(val_is_nil(a[0]) || val_tag(a[0]) == TAG_PAIR);
}
/* map?: 内核尚无 map 值，恒 false（有真实用户时再定语义）。 */
static Val cfunc_mapp(VM *vm, Val *a, int n) { return val_false(); }

/* 名字 ↔ 入口（ta_host_init 按这张表循环注册，名字只有这一处）。元数恒 1：
 * bare_cfunc 白名单里这九个都是一元。 */
static const struct {
    const char *name;
    Val (*fn)(VM *vm, Val *args, int nargs);
} bare_preds[] = {
    {"int?", cfunc_intp}, {"string?", cfunc_stringp}, {"bytes?", cfunc_bytesp},
    {"pid?", cfunc_pidp}, {"float?", cfunc_floatp},   {"bool?", cfunc_boolp},
    {"nil?", cfunc_nilp}, {"list?", cfunc_listp},     {"map?", cfunc_mapp},
};

/* worker 池大小 = NWORKERS 环境变量，默认 1（不设 = 单线程内联，与历史行为
 * 逐字节同路径）。默认关是刻意的：多核是显式开的能力，不是默认行为 ——
 * 所有既有测试/基准的语义基准是单线程，池只加并行度、不改语义。垃圾值/
 * 1/0/负数一律回落默认，不报错；上限由池自己钳（scheduler.c 的
 * MAX_WORKERS），宿主不认识池的界。 */
void ta_host_nworkers(VM *vm) {
    const char *env = getenv("NWORKERS");
    if (!env || !*env)
        return;
    int n = atoi(env);
    if (n > 1)
        vm->nworkers = n;
}

VM *ta_host_init(void) {
    /* 不在 ta.h 里的模块注册入口（同 src/main.c 的静态模块清单）
     * file / os / cov 已下沉为 lazy dylib（首个 file.* / os.raw_* /
     * cov.* 引用时 find_cfunc_autoload 自动 dlopen），不再在此静态注册。 */
    extern void vm_register_num_modules(VM * vm);
    extern void vm_register_vm_module(VM * vm);

    VM *vm = vm_new();
    if (!vm)
        fatal("vm_new failed");
    ta_host_nworkers(vm);
    /* 裸名谓词（主机侧契约见本文件头） */
    for (size_t i = 0; i < sizeof bare_preds / sizeof bare_preds[0]; i++)
        vm_register(vm, bare_preds[i].name, bare_preds[i].fn, 1);
    vm_register_num_modules(vm);
    vm_register_vm_module(vm);
    /* Registry above is host-static from here on: vm.is_builtin_module
     * (import 解析) must keep answering "no source layer" for exactly these
     * names even after a lazy dylib (str/file/os/sexp/buffer) dlopens. */
    vm_seal_host_modules(vm);
    return vm;
}