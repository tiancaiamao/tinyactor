/*
 * main.c — TinyActor CLI: bootstrap script runner
 */

#include "ta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defined in vm.c */
extern void print_val(VM *vm, Val v);

/* Defined in file.c / buf.c / str.c */
extern void vm_register_file_module(VM *vm);
extern void vm_register_buf_module(VM *vm);
extern void vm_register_str_module(VM *vm);

/* Defined in api.c */
extern void vm_register_vm_module(VM *vm);

/* ---- Test module (for module_test.lisp) ---- */

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

static int run_bootstrap(VM *vm, int argc, char **argv) {
    extern void vm_set_argv(int argc, char **argv);
    extern int  vm_load_tabc(VM *vm, const char *path);

    vm_set_argv(argc, argv);

    if (vm_load_tabc(vm, "lib/bootstrap.tabc") != 0) {
        fprintf(stderr, "error: failed to load lib/bootstrap.tabc\n");
        return 1;
    }

    vm_spawn(vm, vm->top_fn_id);

    char *nw = getenv("NWORKERS");
    if (nw) {
        vm->nworkers = atoi(nw);
        if (vm->nworkers < 1) vm->nworkers = 1;
    }

    vm_run(vm);
    return 0;
}

int main(int argc, char **argv) {
    VM *vm = vm_new();

    /* Register test module */
    vm_register_module(vm, "test", test_funcs, 2);

    /* Register net module */
    vm_register_net_module(vm);

    /* Register http module */
    vm_register_http_module(vm);

            /* Register C helper modules */
    vm_register_file_module(vm);
    vm_register_buf_module(vm);
    vm_register_str_module(vm);
    vm_register_vm_module(vm);

    if (argc > 2 && strcmp(argv[1], "--bootstrap") == 0) {
        /* Bootstrap mode: load pre-compiled driver + deps, which compiles
         * and runs the given .ta source file using the Lisp-based compiler. */
        int rc = run_bootstrap(vm, argc, argv);
        vm_free(vm);
        return rc;
    }

    if (argc > 3 && strcmp(argv[1], "--bootstrap-emit") == 0) {
        /* Bootstrap-emit mode: load bootstrap.tabc and use the Lisp compiler
         * to compile a .ta source file into a .tabc file (no execution).
         * This proves self-hosting: the Lisp-based compiler produces a working
         * .tabc without depending on compile.c. */
        int rc = run_bootstrap(vm, argc, argv);
        vm_free(vm);
        return rc;
    }

            /* Fallback: direct .tabc loading (for pre-compiled bytecode) */
    if (argc > 1) {
        int path_len = (int)strlen(argv[1]);
        int is_tabc  = (path_len >= 5 &&
                        strcmp(argv[1] + path_len - 5, ".tabc") == 0);
        if (is_tabc) {
            extern int vm_load_tabc(VM *vm, const char *path);
            if (vm_load_tabc(vm, argv[1]) != 0) {
                fprintf(stderr, "error: failed to load %s\n", argv[1]);
                vm_free(vm);
                return 1;
            }
            vm_spawn(vm, vm->top_fn_id);
            char *nw = getenv("NWORKERS");
            if (nw) {
                vm->nworkers = atoi(nw);
                if (vm->nworkers < 1) vm->nworkers = 1;
            }
            vm_run(vm);
            vm_free(vm);
            return 0;
        }
    }

    /* No recognized args: print usage */
    fprintf(stderr,
        "usage: tinyactor --bootstrap <file>         compile & run .ta via bootstrap\n"
        "       tinyactor --bootstrap-emit <in> <out>  compile .ta to .tabc\n"
        "       tinyactor <file>.tabc                  run pre-compiled bytecode\n");
    vm_free(vm);
    return 1;
}