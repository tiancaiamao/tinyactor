/*
 * http_server.c — HTTP Server using TinyActor VM
 *
 * Build: make http_server
 * Run:   ./http_server
 * Test:  curl http://localhost:8080/
 */

#include "ta.h"
#include <stdio.h>
#include <stdlib.h>

/* Defined in vm.c */
extern void print_val(VM *vm, Val v);

int main(void) {
    VM *vm = vm_new();

    /* Register net and http modules */
    vm_register_net_module(vm);
    vm_register_http_module(vm);

    /* Load the HTTP server script */
    const char *script = "scripts/http_server.lisp";
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