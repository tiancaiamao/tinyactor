/*
 * tavm.c — TinyActor VM: load and run .tabc bytecode.
 *
 * This is the only C binary. Everything else (compile, build, run scripts)
 * is implemented in TA or shell, layered on top of tavm.
 *
 * The C helper modules are statically registered at startup (see main
 * below), so no explicit module pre-loading is needed. Dynamic modules can
 * still be loaded at runtime from TA code:
 *   (vm.load_c_module "lib/demo.so")
 */

#define _DEFAULT_SOURCE /* expose POSIX fileno() under -std=c99 */

#include "ta.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* C helper modules (statically linked) */
extern void vm_register_file_module(VM *vm);
extern void vm_register_os_module(VM *vm);
extern void vm_register_buf_module(VM *vm);
extern void vm_register_cov_module(VM *vm);
extern void vm_register_str_module(VM *vm);
extern void vm_register_num_modules(VM *vm);
extern void vm_register_encoding_module(VM *vm);
extern void vm_register_random_module(VM *vm);
extern void vm_register_vm_module(VM *vm);
extern void vm_register_net_module(VM *vm);
extern void vm_register_tls_module(VM *vm);

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

/* SIGINT → graceful stop: workers notice vm->stop and exit, vm_run returns,
 * and prof_finish dumps the profile. Required for long-running programs
 * (e.g. lib/serve.ta) whose main never returns. */
static VM *g_sig_vm = NULL;
static void on_sigint(int sig) {
    (void)sig;
    if (g_sig_vm)
        atomic_store(&g_sig_vm->stop, 1);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: tavm [--profile[=base]] <file>.tabc [args...]\n"
                        "  --profile       sample at 64-instruction boundaries; write\n"
                        "                  profile.json (speedscope) + profile.folded\n"
                        "  --profile=base  same, with output files <base>.json/.folded\n");
        return 1;
    }

    VM *vm = vm_new();
    /* Parse --profile[=base] */
    int argi = 1;
    const char *prof_out = NULL;
    while (argi < argc) {
        if (strncmp(argv[argi], "--profile", 9) == 0) {
            const char *a = argv[argi];
            if (a[9] == '=' && a[10] != '\0')
                prof_out = a + 10;
            else if (a[9] == '\0')
                prof_out = "profile";
            else {
                fprintf(stderr, "error: unknown option: %s\n", a);
                vm_free(vm);
                return 1;
            }
            argi += 1;
        } else {
            break;
        }
    }

    if (argi >= argc) {
        fprintf(stderr, "usage: tavm [--profile[=base]] <file>.tabc [args...]\n");
        vm_free(vm);
        return 1;
    }

    /* Statically-linked modules */
    vm_register_net_module(vm);
    vm_register_tls_module(vm);
    vm_register_timer_module(vm);
    vm_register_file_module(vm);
    vm_register_os_module(vm);
    vm_register_buf_module(vm);
    vm_register_cov_module(vm);
    vm_register_str_module(vm);
    vm_register_num_modules(vm);
    vm_register_encoding_module(vm);
    vm_register_random_module(vm);
    vm_register_vm_module(vm);

    /* Set argv for TA code: skip -L flags and .tabc path */
    vm_set_argv(argc - argi, argv + argi);

    if (vm_load_tabc(vm, argv[argi]) != 0) {
        fprintf(stderr, "error: failed to load %s\n", argv[argi]);
        vm_free(vm);
        return 1;
    }

    setup_nworkers(vm);
    g_sig_vm = vm;
    signal(SIGINT, on_sigint);
    /* Ignore SIGPIPE process-wide: net.write/read on a reset connection
     * must return EPIPE to TA code, not kill the VM (issue #183). The
     * disposition is set here on the unconditional startup path, before
     * any net.* call can run, and is inherited by every thread (workers
     * and the net resolver included), so no per-call or per-thread
     * signal() is needed. */
    signal(SIGPIPE, SIG_IGN);
    if (prof_out)
        prof_init(vm, prof_out);
    vm_spawn(vm, vm->top_fn_id);
    vm_run(vm);
    g_sig_vm = NULL;
    /* PR #104 diagnostics: dump the global intern table (one "idx name"
     * line per symbol, in intern order) when TA_DUMP_INTERNS is set.
     * Unset → zero effect on normal runs. */
    const char *dump_path = getenv("TA_DUMP_INTERNS");
    if (dump_path && *dump_path) {
        FILE *df = fopen(dump_path, "w");
        if (df) {
            for (int i = 0; i < vm->sym_count; i++) {
                fprintf(df, "%d ", i);
                const char *s = vm->symbols[i];
                if (!s) {
                    fprintf(df, "(null)");
                } else {
                    for (const unsigned char *c = (const unsigned char *)s; *c; c++)
                        fprintf(df, "%c", (*c >= 32 && *c < 127) ? *c : '?');
                }
                fprintf(df, "\n");
            }
            fclose(df);
        }
    }
    /* Read the crash flag BEFORE vm_free: proc_die sets main_crashed when
     * the main process dies abnormally (reason != nil); a normal exit or a
     * non-main actor crash keeps it 0 → exit code 0, as before. */
    int main_crashed = atomic_load(&vm->main_crashed);
    if (prof_out)
        prof_finish(vm);
    fflush(stdout);
    fsync(fileno(stdout));
    vm_free(vm);
    return main_crashed ? 1 : 0;
}