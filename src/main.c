/*
 * main.c — TinyActor CLI: script runner and REPL
 */

#include "ta.h"
#include <stdio.h>
#include <stdlib.h>

/* Defined in vm.c */
extern void print_val(VM *vm, Val v);

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

int main(int argc, char **argv) {
    VM *vm = vm_new();

    /* Register test module */
    vm_register_module(vm, "test", test_funcs, 2);

    /* Register net module */
    vm_register_net_module(vm);

    if (argc > 1) {
        /* Script mode */
        if (vm_load_file(vm, argv[1]) != 0) {
            fprintf(stderr, "error: failed to load %s\n", argv[1]);
            vm_free(vm);
            return 1;
        }
                                                /* Top-level thunk is the last fn_id */
        vm_spawn(vm, vm->top_fn_id);

        /* Optional worker count override: NWORKERS=N */
        char *nw = getenv("NWORKERS");
        if (nw) {
            vm->nworkers = atoi(nw);
            if (vm->nworkers < 1) vm->nworkers = 1;
        }

        vm_run(vm);
    } else {
        /* REPL */
        char line[4096];
        fprintf(stderr, "tinyactor> ");
        while (fgets(line, sizeof(line), stdin)) {
            if (line[0] == '\n') {
                fprintf(stderr, "tinyactor> ");
                continue;
            }
            Val result = vm_eval(vm, line);
            print_val(vm, result);
            printf("\n");
            fprintf(stderr, "tinyactor> ");
        }
    }

    vm_free(vm);
    return 0;
}