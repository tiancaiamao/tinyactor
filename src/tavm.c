/*
 * tavm.c — TinyActor VM: load and run .tabc bytecode.
 *
 * This is the only C binary. Everything else (compile, build, run scripts)
 * is implemented in TA or shell, layered on top of tavm.
 *
 * IMPORTANT: C function registration order must remain stable — the
 * .tabc files on disk embed CCALL indices that were resolved against
 * this exact ordering at compile time.  See vm_register_* calls below.
 */

#include "ta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* C helper modules */
extern void vm_register_file_module(VM *vm);
extern void vm_register_buf_module(VM *vm);
extern void vm_register_str_module(VM *vm);
extern void vm_register_vm_module(VM *vm);
extern void vm_register_net_module(VM *vm);
extern void vm_register_http_module(VM *vm);

/* Forward declarations from api.c */
extern void vm_set_argv(int argc, char **argv);
extern int  vm_load_tabc(VM *vm, const char *path);

/* ---- Test module (kept for cfunc index compatibility with old bootstrap.tabc) ---- */
static Val test_hello(VM *vm, Val *args, int nargs) {
    (void)vm; (void)args; (void)nargs;
    return val_string(tls_current_proc, "hello from C", 12);
}

static Val test_add(VM *vm, Val *args, int nargs) {
    (void)vm;
    if (nargs < 2) return val_int(0);
    return val_int(val_get_int(args[0]) + val_get_int(args[1]));
}

static TaFunc test_funcs[] = {
    {"hello", test_hello, 0},
    {"add",   test_add,   2},
    {NULL, NULL, 0}
};

static void setup_nworkers(VM *vm) {
    char *nw = getenv("NWORKERS");
    if (nw) {
        vm->nworkers = atoi(nw);
        if (vm->nworkers < 1) vm->nworkers = 1;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: tavm <file>.tabc [args...]\n");
        return 1;
    }

    VM *vm = vm_new();

    /* Registration order MUST match the order used when the .tabc files
     * on disk were compiled.  Changing this shifts CCALL indices. */
    vm_register_module(vm, "test", test_funcs, 2);   /*  0- 1 */
    vm_register_net_module(vm);                       /*  2- 7 */
    vm_register_http_module(vm);                      /*  8- 9 */
    vm_register_file_module(vm);                      /* 10-12 */
    vm_register_buf_module(vm);                       /* 13-22 */
    vm_register_str_module(vm);                       /* 23-32 */
    vm_register_vm_module(vm);                        /* 33-44 */

    /* Don't pass .tabc path to VM; start at first user arg */
    vm_set_argv(argc - 1, argv + 1);

    if (vm_load_tabc(vm, argv[1]) != 0) {
        fprintf(stderr, "error: failed to load %s\n", argv[1]);
        vm_free(vm);
        return 1;
    }

    setup_nworkers(vm);
    vm_spawn(vm, vm->top_fn_id);
    vm_run(vm);
    vm_free(vm);
    return 0;
}