/*
 * lib/demo.c — Minimal C module template (E3).
 *
 * 照着这个文件写你自己的 C 模块：
 *   1. #include "ta.h"（公开 API：Val、Proc、val_* 构造/访问器）
 *   2. 每个函数签名：static Val fn(VM *vm, Proc *p, Val *args, int nargs)
 *   3. 导出一个 TaFunc 表：{名字, 函数, 参数个数}，-1 = 变参
 *   4. 实现 void vm_load_self(VM *vm)，调用 vm_register_module
 *
 * 编译（Makefile 已有规则）：
 *   make lib/demo.dylib
 *
 * TA 侧用法（无需 import，装载 .tabc 时自动 dlopen 加载）：
 *   print(demo.double(21))   ; 42
 *   print(demo.greet("ta"))  ; hello ta
 *
 * 完整契约（错误约定、GC 心智模型、类型映射）见 docs/c-module.md。
 */
#include "ta.h"

#include <stdlib.h>
#include <string.h>

/* 模式 1：纯 int 运算 —— 不分配堆对象，GC 无感知，最简单。 */
static Val demo_double(VM *vm, Proc *p, Val *args, int nargs) {
    if (!val_is_int(args[0]))
        return val_nil(); /* 错误约定：失败返回 nil */
    return val_int(val_get_int(args[0]) * 2);
}

/* 模式 2：构造 string —— 单次分配即返回，无需手动 root。
 * val_string 把内容拷贝进 proc heap（由 GC 管理）；我们自己
 * malloc 的临时 buffer 必须自己 free。 */
static Val demo_greet(VM *vm, Proc *p, Val *args, int nargs) {
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

/* 模式 3：构造复合值（两元 list）—— 多步分配不需要任何保护：CCALL（do_ccall）
 * 把整个 C 回调罩在 gc_gate 门内，回调期间不发生 GC（此处的分配只置
 * gc_pending，要等结果压回 TA 栈后才补收）；且 actor arena 一旦持有对象就不再
 * realloc（见 docs/c-module.md §3）。第二步分配在前一步之后，所以「先算后分」的
 * 坑不会出现。 */
static Val demo_list(VM *vm, Proc *p, Val *args, int nargs) {
    return val_pair(p, args[0], val_pair(p, args[1], val_nil()));
}

TaFunc demo_funcs[] = {
    {"double", demo_double, 1},
    {"greet", demo_greet, 1},
    {"list", demo_list, 2},
    {NULL, NULL, 0},
};

void vm_load_self(VM *vm) { vm_register_module(vm, "demo", demo_funcs, 3); }