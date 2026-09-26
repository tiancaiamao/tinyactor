/*
 * cov.c — coverage counter sink for TinyActor VM
 *
 * Counterpart of the compiler's --cov mode (lib/bootstrap/cov.ta): the
 * driver rewrites each top-level fn body to begin with (cov.hit k) and
 * emits a <out>.covmap side table (k -> fn name). This module is the
 * runtime sink: a growable counter array shared by all actors (the
 * Erlang-cover/ETS analogue — a pure message-passing sink would be
 * async and lossy at crash time).
 *
 *   cov.hit(id)      -> Int   (1 counted, 0 bad id)
 *   cov.dump(path)   -> Int   (1 wrote "id count" lines, 0 on error)
 *   cov.reset()      -> Int   (1, counters zeroed)
 *
 * Deliberately NOT an opcode: (cov.hit k) is an ordinary qualified
 * ccall, so the VM, TABC format and typechecker are untouched. The
 * counters are process-global (like buf.c's table): tavm runs one VM
 * per process, and the mutex makes hits safe under multi-threaded
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
    for (int i = 0; i < cov_high; i++)
        fprintf(f, "%d %llu\n", i, cov_counts[i]);
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

/* Optional per-process dump used by `make coverage-ta`. The directory
 * is set only for the test run; PID-qualified files avoid cross-process
 * races when test categories run concurrently. */
void cov_dump_env(const char *dir) {
    if (!dir || !*dir)
        return;
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/%ld-XXXXXX", dir, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof path)
        return;
    int fd = mkstemp(path);
    if (fd < 0)
        return;
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(path);
        return;
    }
    pthread_mutex_lock(&cov_lock);
    for (int i = 0; i < cov_high; i++)
        fprintf(f, "%d %llu\n", i, cov_counts[i]);
    fclose(f);
    pthread_mutex_unlock(&cov_lock);
}

TaFunc cov_funcs[] = {
    {"hit", cov_hit, 1}, {"dump", cov_dump, 1}, {"reset", cov_reset, 0}, {NULL, NULL, 0}};

void vm_register_cov_module(VM *vm) { vm_register_module(vm, "cov", cov_funcs, 3); }