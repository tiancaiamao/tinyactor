/*
 * echo_server.c — TCP Echo Server using TinyActor VM
 *
 * Build: make echo_server
 * Run:   ./echo_server
 * Test:  echo "hello" | nc -w 2 localhost 8090
 */

#include "ta.h"
#include <stdio.h>
#include <stdlib.h>

/* Defined in vm.c */
extern void print_val(VM *vm, Val v);

int main(void) {
    VM *vm = vm_new();

    /* Register net module */
    vm_register_net_module(vm);

    /* Load the echo server script */
        const char *script = "scripts/echo_server.lisp";
    if (vm_load_file(vm, script) != 0) {
        fprintf(stderr, "error: failed to load %s\n", script);
        vm_free(vm);
        return 1;
    }

    /* Spawn top-level thunk and run */
    vm_spawn(vm, vm->top_fn_id);
    vm_run(vm);

    vm_free(vm);
    return 0;
}