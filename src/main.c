/*
 * main.c — TinyActor CLI: script runner and one-shot eval
 *
 * Default mode: load bootstrap.tabc and use the TA compiler for .ta files.
 * The C compiler (compile.c) is only reachable via explicit --c-compile flag
 * and is kept for bootstrap generation and verification during transition.
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

/* Helper: compute .tabc output path from source path */
static void tabc_out_path(const char *src_path, char *out, int out_sz) {
    int len = (int)strlen(src_path);
    int base = len;
    if      (len >= 5 && strcmp(src_path + len - 5, ".lisp") == 0) base = len - 5;
    else if (len >= 5 && strcmp(src_path + len - 5, ".tabc") == 0) base = len - 5;
    else if (len >= 3 && strcmp(src_path + len - 3, ".ta") == 0)   base = len - 3;
    snprintf(out, out_sz, "%.*s.tabc", base, src_path);
}

static int is_tabc(const char *path) {
    int len = (int)strlen(path);
    return (len >= 5 && strcmp(path + len - 5, ".tabc") == 0);
}

/* Load and run via bootstrap.tabc (TA compiler).
 * Sets up argv so the TA compiler's main() finds the file. */
static int run_ta_path(VM *vm, int argc, char **argv) {
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

/* Load and run via C compiler path (compile.c).
 * Used only for --c-compile flag during transition. */
static int run_c_path(VM *vm, const char *path, int emit_tabc) {
    extern int vm_load_file(VM *vm, const char *path);
    extern int vm_dump_tabc(VM *vm, const char *path);

    if (vm_load_file(vm, path) != 0) {
        fprintf(stderr, "error: failed to load %s\n", path);
        return 1;
    }

    if (emit_tabc) {
        char outpath[512];
        tabc_out_path(path, outpath, sizeof(outpath));
        if (vm_dump_tabc(vm, outpath) != 0) {
            fprintf(stderr, "error: failed to write %s\n", outpath);
            return 1;
        }
        printf("wrote %s\n", outpath);
        return 0;
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

    /* ============================================================
     * Explicit C path (transition period only)
     * ============================================================ */
    if (argc > 2 && strcmp(argv[1], "--c-compile") == 0) {
        /* Run a .ta file through the C compiler path.
         * Usage: tinyactor --c-compile <file>.ta [--emit-tabc] */
        int emit = (argc > 3 && strcmp(argv[3], "--emit-tabc") == 0);
        int ret = run_c_path(vm, argv[2], emit);
        vm_free(vm);
        return ret;
    }

    /* ============================================================
     * Bootstrap flags (TA path, explicit)
     * ============================================================ */
    if (argc > 2 && strcmp(argv[1], "--bootstrap") == 0) {
        /* Usage: tinyactor --bootstrap <file>.ta [--check] */
        run_ta_path(vm, argc, argv);
        vm_free(vm);
        return 0;
    }

    if (argc > 3 && strcmp(argv[1], "--bootstrap-emit") == 0) {
        /* Usage: tinyactor --bootstrap-emit <file>.ta <out>.tabc */
        run_ta_path(vm, argc, argv);
        vm_free(vm);
        return 0;
    }

        /* ============================================================
     * --eval flag: compile and run a single expression
     * Must come before default path check so --eval "expr" works.
     * ============================================================ */
    if (argc > 2 && strcmp(argv[1], "--eval") == 0) {
        Val result = vm_eval(vm, argv[2]);
        print_val(vm, result);
        printf("\n");
        vm_free(vm);
        return 0;
    }

    /* ============================================================
     * Default mode: TA path for .ta files, bytecode loader for .tabc
     * ============================================================ */
    if (argc > 1) {
        if (is_tabc(argv[1])) {
            /* Load pre-compiled bytecode directly */
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

        /* --emit-tabc flag: translate to TA compiler's compile_file_to_tabc */
        if (argc > 2 && strcmp(argv[2], "--emit-tabc") == 0) {
            char outpath[512];
            tabc_out_path(argv[1], outpath, sizeof(outpath));
            /* Fake argv for TA compiler: --bootstrap-emit <file> <out> */
            char *fake_argv[5];
            fake_argv[0] = argv[0];
            fake_argv[1] = "--bootstrap-emit";
            fake_argv[2] = argv[1];
            fake_argv[3] = outpath;
            fake_argv[4] = NULL;

            int ret = run_ta_path(vm, 4, fake_argv);
            vm_free(vm);
            return ret;
        }

                        /* Default: compile and run .ta via TA compiler */
        {
            int ret = run_ta_path(vm, argc, argv);
            vm_free(vm);
            return ret;
        }
    }

    /* No args: print usage */
    fprintf(stderr,
        "usage: tinyactor <file>.ta         compile and run via TA compiler (bootstrap.tabc)\n"
        "       tinyactor <file>.tabc        run pre-compiled bytecode\n"
        "       tinyactor <file>.ta --emit-tabc  compile to bytecode via TA compiler\n"
        "       tinyactor --bootstrap ...    alias for default TA path\n"
        "       tinyactor --bootstrap-emit <in> <out> TA compile to explicit output\n"
        "       tinyactor --c-compile <file> force C compiler path (transition only)\n"
        "       tinyactor --eval \"<expr>\"   run a single expression\n");
    vm_free(vm);
    return 0;
}