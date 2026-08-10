/*
 * tavm.c — TinyActor VM: load and run .tabc bytecode.
 *
 * This is the only C binary. Everything else (compile, build, run scripts)
 * is implemented in TA or shell, layered on top of tavm.
 *
 * The http C module is auto-loaded at startup (see main below), so no
 * explicit module pre-loading is needed. Dynamic modules can still be
 * loaded at runtime from TA code:
 *   (vm.load_c_module "lib/http.so")
 */

#define _DEFAULT_SOURCE /* expose POSIX fileno() under -std=c99 */

#include "ta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* C helper modules (statically linked) */
extern void vm_register_file_module(VM *vm);
extern void vm_register_buf_module(VM *vm);
extern void vm_register_str_module(VM *vm);
extern void vm_register_vm_module(VM *vm);
extern void vm_register_net_module(VM *vm);

/* Forward declarations from api.c */
extern void vm_set_argv(int argc, char **argv);
extern int vm_load_tabc(VM *vm, const char *path);
extern int vm_load_c_module(VM *vm, const char *path);

static void setup_nworkers(VM *vm) {
    char *nw = getenv("NWORKERS");
    if (nw) {
        vm->nworkers = atoi(nw);
        if (vm->nworkers < 1)
            vm->nworkers = 1;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: tavm <file>.tabc [args...]\n");
        return 1;
    }

    VM *vm = vm_new();
    int argi = 1;

    /* Statically-linked modules */
    vm_register_net_module(vm);
    vm_register_file_module(vm);
    vm_register_buf_module(vm);
    vm_register_str_module(vm);
    vm_register_vm_module(vm);

    /* http is a dynamically-loaded C module (lib/http.dylib on macOS,
     * lib/http.so on Linux, tagged _asan/_tsan for sanitizer builds).
     * Load it up front — like the statically-linked modules — so
     * `import http` and http.* calls compile and run without any flag.
     * The module name is registered even if the dylib is missing, so
     * imports still typecheck (functions then auto-load lazily on the
     * first http.* call via vm.c). */
    vm_register_module(vm, "http", NULL, 0);
#ifdef __APPLE__
    const char *http_ext = "dylib";
#else
    const char *http_ext = "so";
#endif
    char http_mod[64];
#ifdef TA_MOD_TAG
    snprintf(http_mod, sizeof(http_mod), "lib/http_%s.%s", TA_MOD_TAG_STR(TA_MOD_TAG), http_ext);
#else
    snprintf(http_mod, sizeof(http_mod), "lib/http.%s", http_ext);
#endif
    vm_load_c_module(vm, http_mod);

    /* Set argv for TA code: skip -L flags and .tabc path */
    vm_set_argv(argc - argi, argv + argi);

    if (vm_load_tabc(vm, argv[argi]) != 0) {
        fprintf(stderr, "error: failed to load %s\n", argv[argi]);
        vm_free(vm);
        return 1;
    }

    setup_nworkers(vm);
    vm_spawn(vm, vm->top_fn_id);
    vm_run(vm);
    fflush(stdout);
    fsync(fileno(stdout));
    vm_free(vm);
    return 0;
}