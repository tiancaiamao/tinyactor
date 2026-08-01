/*
 * tavm.c — TinyActor VM: load and run .tabc bytecode.
 *
 * This is the only C binary. Everything else (compile, build, run scripts)
 * is implemented in TA or shell, layered on top of tavm.
 *
 * Dynamic C modules can be pre-loaded via -L:
 *   tavm -L lib/http.so my_program.tabc [args...]
 *
 * Or loaded at runtime from TA code:
 *   (vm.load_c_module "lib/http.so")
 */

#define _DEFAULT_SOURCE  /* expose POSIX fileno() under -std=c99 */

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
        fprintf(stderr, "usage: tavm [-L <module.so>...] <file>.tabc [args...]\n");
        return 1;
    }

    VM *vm = vm_new();

    /* Parse -L flags to pre-load dynamic modules */
    int argi = 1;
    while (argi < argc && strcmp(argv[argi], "-L") == 0) {
        if (argi + 1 >= argc) {
            fprintf(stderr, "error: -L requires a path argument\n");
            vm_free(vm);
            return 1;
        }
        const char *mod_path = argv[argi + 1];
        if (vm_load_c_module(vm, mod_path) != 0) {
            fprintf(stderr, "error: failed to load module: %s\n", mod_path);
            vm_free(vm);
            return 1;
        }
        argi += 2;
    }

    if (argi >= argc) {
        fprintf(stderr, "usage: tavm [-L <module.so>...] <file>.tabc [args...]\n");
        vm_free(vm);
        return 1;
    }

    /* Statically-linked modules */
    vm_register_net_module(vm);
    vm_register_file_module(vm);
    vm_register_buf_module(vm);
    vm_register_str_module(vm);
    vm_register_vm_module(vm);

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