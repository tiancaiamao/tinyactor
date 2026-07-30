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

/* ---- C module registration (externs) ---- */
extern void vm_register_net_module(VM *vm);
extern void vm_register_http_module(VM *vm);
extern void vm_register_file_module(VM *vm);
extern void vm_register_buf_module(VM *vm);
extern void vm_register_str_module(VM *vm);
extern void vm_register_vm_module(VM *vm);

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

        /* Registration order no longer matters for CCALL — name-based dispatch
     * (OP_CCALL_NAME) resolves at runtime. Kept sorted for readability. */
    vm_register_net_module(vm);                       /*  0- 5 */
    vm_register_http_module(vm);                      /*  6- 7 */
    vm_register_file_module(vm);                      /*  8-10 */
    vm_register_buf_module(vm);                       /* 11-20 */
    vm_register_str_module(vm);                       /* 21-30 */
    vm_register_vm_module(vm);                        /* 31-42 */

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