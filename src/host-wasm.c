/* host-wasm.c — wasm 里静态链接进 VM 的那几个 C 模块。
 *
 * 原生构建里 str / file / os 是 lazy dylib（lib/<mod>.c，首个引用时
 * find_cfunc_autoload 走 dlopen）。wasm 不开 emscripten 动态链接，dlopen
 * 一个也进不来，而 Playground 里的编译器自己读源码、拼字符串、出错退出全靠
 * 这三个 —— 所以 build-wasm.sh 把它们直接链进 VM，在这里注册。
 *
 * 表里的符号由 build-wasm.sh 的编译参数造出来：每个 lib/<mod>.c 都导出
 * 同名入口 vm_load_self，静态链接会撞车，所以那一遍编译用
 *   -Dvm_load_self=ta_wasm_<mod>_load_self
 * 改名。加模块 = 这里加一行 + build-wasm.sh 的 WASM_C_MODULES 加一个目录名，
 * 两处名字必须一致（漏改的报错是链接期 undefined symbol，不是静默失效）。
 *
 * 原生构建编到本文件时是空 TU：没有这条路径，模块照样按需 dlopen。
 */
#ifdef __EMSCRIPTEN__

#include "ta.h"

void ta_wasm_str_load_self(VM *vm);
void ta_wasm_buffer_load_self(VM *vm);
void ta_wasm_file_load_self(VM *vm);
void ta_wasm_os_load_self(VM *vm);

void ta_host_wasm_static_modules(VM *vm) {
    ta_wasm_str_load_self(vm);
    ta_wasm_buffer_load_self(vm);
    ta_wasm_file_load_self(vm);
    ta_wasm_os_load_self(vm);
}

#else

typedef int ta_host_wasm_unused; /* 空 TU 的显式说明：原生构建不走这里 */

#endif /* __EMSCRIPTEN__ */