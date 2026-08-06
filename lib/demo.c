/*
 * lib/demo.c — Minimal C module template (E3).
 *
 * 照着这个文件写你自己的 C 模块：
 *   1. #include "ta.h"（公开 API：Val、Proc、val_* 构造/访问器）
 *   2. 每个函数签名：static Val fn(VM *vm, Val *args, int nargs)
 *   3. 导出一个 TaFunc 表：{名字, 函数, 参数个数}，-1 = 变参
 *   4. 实现 void vm_load_self(VM *vm)，调用 vm_register_module
 *
 * 编译（Makefile 已有规则）：
 *   make lib/demo.dylib
 *
 * TA 侧用法（无需 import，首次调用时运行时自动 dlopen 加载）：
 *   print(demo.double(21))   ; 42
 *   print(demo.greet("ta"))  ; hello ta
 *
 * 完整契约（错误约定、GC 心智模型、类型映射）见 docs/c-module.md。
 */
#include "ta.h"

#include <stdlib.h>
#include <string.h>

/* 模式 1：纯 int 运算 —— 不分配堆对象，GC 无感知，最简单。 */
static Val demo_double(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_int(args[0]))
        return val_nil(); /* 错误约定：失败返回 nil */
    return val_int(val_get_int(args[0]) * 2);
}

/* 模式 2：构造 string —— 单次分配即返回，无需手动 root。
 * val_string 把内容拷贝进 proc heap（由 GC 管理）；我们自己
 * malloc 的临时 buffer 必须自己 free。 */
static Val demo_greet(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    Proc *p = tls_current_proc; /* 当前执行 proc（GC 根） */
    if (!val_is_string(args[0]))
        return val_nil();
    HeapString *name = val_get_string(args[0]);
    const char *prefix = "hello ";
    int total = (int)strlen(prefix) + name->len;
    char *buf = malloc(total);
    if (!buf)
        return val_nil();
    memcpy(buf, prefix, strlen(prefix));
    memcpy(buf + strlen(prefix), name->data, name->len);
    Val s = val_string(p, buf, total); /* 拷贝进堆 → GC 接管 */
    free(buf);
    return s;
}

/* 模式 3：构造 pair —— val_pair 内部已自动保护多步分配。
 * 跨多次分配的场景用 GC_ROOTS_SCOPE(p, rbase)（见 ta.h）。 */
static Val demo_pair(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    Proc *p = tls_current_proc;
    return val_pair(p, args[0], args[1]);
}

TaFunc demo_funcs[] = {
    {"double", demo_double, 1},
    {"greet", demo_greet, 1},
    {"pair", demo_pair, 2},
    {NULL, NULL, 0},
};

void vm_load_self(VM *vm) { vm_register_module(vm, "demo", demo_funcs, 3); }