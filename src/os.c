/*
 * os.c — Process environment module for TinyActor VM
 *
 *   os.getenv(name) -> String | nil  (nil when unset or bad arg)
 *   os.args()       -> list of String (argv as seen by the VM: argv[0] is
 *                      the executed script path, remaining entries are the
 *                      args after it)
 *   os.exit(code)   -> never returns (terminates the whole VM process)
 *   os.hostname()   -> String | -1    (-1 = hard error, see below)
 *
 * Status convention (docs/c-module.md signal vocabulary): -1 is the hard
 * error signal (syscall failure); lib/os.ta lifts it to Result/Option.
 * getenv reports "unset" as nil (absence is not a failure); the TA layer
 * lifts nil to Option.None.
 *
 * Statically registered in tavm.c (same as the file module), so no dylib
 * build step is needed.
 */

#include "ta.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* argv accessor from src/api.c (set by tavm.c main via vm_set_argv). */
extern void vm_get_argv(int *argc, char ***argv);

static Val os_getenv(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_string(args[0]))
        return val_nil(); /* absence, not failure */
    HeapString *hs = val_get_string(args[0]);
    const char *v = getenv(hs->data); /* hs->data is NUL-terminated */
    if (!v)
        return val_nil();
    return val_string(tls_current_proc, v, (int)strlen(v));
}

static Val os_args(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)args;
    (void)nargs;
    int argc = 0;
    char **argv = NULL;
    vm_get_argv(&argc, &argv);
    /* Build the list right-to-left so prepending yields argv order. */
    Val list = val_nil();
    for (int i = argc - 1; i >= 0; i--) {
        if (!argv[i])
            continue;
        list = val_pair(tls_current_proc,
                        val_string(tls_current_proc, argv[i], (int)strlen(argv[i])), list);
    }
    return list;
}

static Val os_exit(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    exit(val_is_int(args[0]) ? (int)val_get_int(args[0]) : 1);
    /* not reached */
    return val_nil();
}

static Val os_hostname(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)args;
    (void)nargs;
    char buf[256];
    if (gethostname(buf, sizeof(buf)) != 0)
        return val_int(-1);
    buf[sizeof(buf) - 1] = '\0'; /* gethostname may truncate unterminated */
    return val_string(tls_current_proc, buf, (int)strlen(buf));
}

TaFunc os_funcs[] = {{"raw_getenv", os_getenv, 1},
                     {"raw_args", os_args, 0},
                     {"raw_exit", os_exit, 1},
                     {"raw_hostname", os_hostname, 1},
                     {NULL, NULL, 0}};

/* Register the raw primitives under their full dotted names, but WITHOUT a
 * vm_register_module("os") entry: a module-registry entry would make
 * `import os` a compile-time no-op (is_builtin_module) and lib/os.ta — the
 * Option/Result lift layer that owns the public API — would never load.
 * The TA module os owns the public namespace (getenv/args/exit/hostname);
 * C owns only the os.raw_* primitives. (Same reasoning as buffer's lazy
 * dylib, see Makefile.) */
void vm_register_os_module(VM *vm) {
    for (int i = 0; os_funcs[i].name != NULL; i++) {
        char qualified[64];
        snprintf(qualified, sizeof(qualified), "os.%s", os_funcs[i].name);
        vm_register(vm, qualified, os_funcs[i].fn, os_funcs[i].nargs);
    }
}