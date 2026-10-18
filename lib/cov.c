/*
 * cov.c — coverage counter sink for TinyActor VM
 *
 * Standalone coverage counter sink, loaded as the `cov` dylib module
 * (lazy dlopen on first cov.* reference, vm_load_self below). This module is the runtime
 * sink: a growable counter array shared by all actors (the
 * Erlang-cover/ETS analogue — a pure message-passing sink would be
 * async and lossy at crash time). This module is the runtime half of the
 * compiler's --cov mode: --cov instruments an image with (cov.hit k)
 * calls (lib/bootstrap/covinst.ta) plus a <out>.covmap side table, and
 * every process running such an image dumps its counters at exit when
 * TA_COV_DUMP_DIR is set — one env var, no VM/CLI change and no injected
 * call, so the hook covers the bootstrap driver process and every test
 * program alike.
 *
 *   cov.hit(id)      -> Int   (1 counted, 0 bad id)
 *   cov.dump(path)   -> Int   (1 wrote "id count" lines, 0 on error)
 *   cov.reset()      -> Int   (1, counters zeroed)
 *
 * Deliberately NOT an opcode: (cov.hit k) is an ordinary qualified
 * ccall, so the VM, TABC format and typechecker are untouched. The
 * counters are process-global (like lib/buffer.c's table): the VM runs
 * one per process, and the mutex makes hits safe under multi-threaded
 * scheduling. Zero cost when unused — nothing grows until hit.
 */

#include "ta.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static unsigned long long *cov_counts;
static int cov_cap = 0;
static int cov_high = 0; /* one past the highest initialized slot */
static pthread_mutex_t cov_lock = PTHREAD_MUTEX_INITIALIZER;

static Val cov_hit(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_int(args[0]))
        return val_int(0);
    long long id = val_get_int(args[0]);
    if (id < 0)
        return val_int(0);
    pthread_mutex_lock(&cov_lock);
    if (id >= cov_cap) {
        int new_cap = cov_cap ? cov_cap : 64;
        while (new_cap <= id)
            new_cap *= 2;
        unsigned long long *nc = realloc(cov_counts, (size_t)new_cap * sizeof *nc);
        if (!nc) {
            pthread_mutex_unlock(&cov_lock);
            return val_int(0);
        }
        cov_counts = nc;
        cov_cap = new_cap;
    }
    while (cov_high <= id)
        cov_counts[cov_high++] = 0;
    cov_counts[id]++;
    pthread_mutex_unlock(&cov_lock);
    return val_int(1);
}

/* Write every initialized counter as an "id count" line. Caller holds
 * cov_lock. */
static void cov_write(FILE *f) {
    for (int i = 0; i < cov_high; i++)
        fprintf(f, "%d %llu\n", i, cov_counts[i]);
}

/* Exit dump: with TA_COV_DUMP_DIR set, write <dir>/cov-<pid>.cov.
 * One file per process — a coverage run spawns hundreds of tavm
 * processes sharing one dump dir, and the pid keeps them from clobbering
 * each other. A dir that cannot be written is reported, not silent: a run
 * that quietly lost its dumps would report everything as uncovered. */
static void cov_dump_env(void) {
    const char *dir = getenv("TA_COV_DUMP_DIR");
    if (!dir || !*dir)
        return;
    char path[1024];
    int n = snprintf(path, sizeof path, "%s/cov-%ld.cov", dir, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof path) {
        fprintf(stderr, "cov: dump path too long: %s\n", dir);
        return;
    }
    pthread_mutex_lock(&cov_lock);
    FILE *f = fopen(path, "w");
    if (f) {
        cov_write(f);
        fclose(f);
    } else {
        fprintf(stderr, "cov: cannot write %s\n", path);
    }
    pthread_mutex_unlock(&cov_lock);
}

static Val cov_dump(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_string(args[0]))
        return val_int(0);
    HeapString *hs = val_get_string(args[0]);
    pthread_mutex_lock(&cov_lock);
    FILE *f = fopen(hs->data, "w");
    if (!f) {
        pthread_mutex_unlock(&cov_lock);
        return val_int(0);
    }
    cov_write(f);
    int ok = fclose(f) == 0;
    pthread_mutex_unlock(&cov_lock);
    return val_int(ok ? 1 : 0);
}

static Val cov_reset(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    pthread_mutex_lock(&cov_lock);
    for (int i = 0; i < cov_high; i++)
        cov_counts[i] = 0;
    pthread_mutex_unlock(&cov_lock);
    return val_int(1);
}

TaFunc cov_funcs[] = {
    {"hit", cov_hit, 1}, {"dump", cov_dump, 1}, {"reset", cov_reset, 0}, {NULL, NULL, 0}};

/* Dynamic module entry: dlsym("vm_load_self") after dlopen (main.c
 * find_cfunc_autoload). Registers the cov.* module so the 3 primitives
 * resolve by name, and arms the exit dump: the handler is a no-op unless
 * TA_COV_DUMP_DIR is set when the process exits, so a coverage run is
 * switched on entirely from the outside (env), never by the program. */
void vm_load_self(VM *vm) {
    vm_register_module(vm, "cov", cov_funcs, 3);
    atexit(cov_dump_env);
}