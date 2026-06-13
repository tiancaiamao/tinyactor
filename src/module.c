/*
 * module.c — Module registration for TinyActor VM
 *
 * Registers module functions as "module.funcname" in the cfunc table,
 * allowing (import "name") + module.func() calls from Lisp.
 */

#include "ta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void vm_register_module(VM *vm, const char *name,
                        TaFunc *funcs, int nfuncs) {
    /* Track in module registry */
    if (vm->mod_count >= vm->mod_cap) {
        vm->mod_cap = vm->mod_cap ? vm->mod_cap * 2 : 16;
        vm->mod_funcs  = realloc(vm->mod_funcs,  vm->mod_cap * sizeof(TaFunc *));
        vm->mod_nfuncs = realloc(vm->mod_nfuncs, vm->mod_cap * sizeof(int));
        vm->mod_names  = realloc(vm->mod_names,  vm->mod_cap * sizeof(char *));
    }
    vm->mod_funcs[vm->mod_count]  = funcs;
    vm->mod_nfuncs[vm->mod_count] = nfuncs;
    vm->mod_names[vm->mod_count]  = strdup(name);
    vm->mod_count++;

    /* Register each function as "module.funcname" in cfunc table */
    for (int i = 0; i < nfuncs; i++) {
        int qlen = (int)(strlen(name) + 1 + strlen(funcs[i].name) + 1);
        char *qualified = malloc(qlen);
        snprintf(qualified, qlen, "%s.%s", name, funcs[i].name);
        vm_register(vm, qualified, funcs[i].fn, funcs[i].nargs);
        free(qualified);
    }
}