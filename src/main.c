/*
 * main.c — TinyActor CLI: script runner and REPL
 */

#include "ta.h"
#include <stdio.h>
#include <stdlib.h>

/* Defined in vm.c */
extern void print_val(VM *vm, Val v);

int main(int argc, char **argv) {
    VM *vm = vm_new();

    if (argc > 1) {
        /* Script mode */
        if (vm_load_file(vm, argv[1]) != 0) {
            fprintf(stderr, "error: failed to load %s\n", argv[1]);
            vm_free(vm);
            return 1;
        }
                        /* Top-level thunk is the last fn_id */
        vm_spawn(vm, vm->top_fn_id);
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