/*
 * lib/time.c — time module for TinyActor (built as lib/time.dylib / .so)
 *
 * Backs the external fn declarations in lib/time.ta (demo module pattern:
 * lazily dlopen'd on the first time.* cfunc call, so `import time` keeps
 * resolving lib/time.ta as a strict TA module).
 *
 *   now_ms()       CLOCK_REALTIME wall-clock milliseconds
 *   monotonic_ms() CLOCK_MONOTONIC milliseconds — same clock source as
 *                  vm.time_ms (src/api.c); reimplemented here because a
 *                  lazy dylib cannot reach api.c's static function.
 *   sleep(ms)      see SLEEP MECHANISM below. Returns 0, or -1 on a
 *                  non-int argument (hard error, docs/c-module.md §4).
 *
 * SLEEP MECHANISM (v1, documented per lib/time.ta header):
 *   blocking nanosleep inside the OP_CCALL callback. This BLOCKS THE
 *   WORKER THREAD for the full duration — every other actor on the same
 *   worker stalls, and each concurrent sleeper pins one worker (run with
 *   NWORKERS>=sleepers for parallel sleeps). Chosen over the existing
 *   scheduler mechanisms because none is a faithful sleep:
 *   - recv_after(ms) is deadline-armed and does not block a worker, but
 *     it is mailbox-bound: a message arriving mid-wait wakes the caller
 *     early AND consumes the message. timer-sleep semantics (Erlang
 *     `receive after T -> ok end`) must leave the mailbox untouched and
 *     wait the full duration.
 *   - a mailbox-independent wait-deadline would be a new scheduler
 *     primitive (src/scheduler.c change — out of scope for this module).
  *   The blocking cost is bounded and visible; revisit if real workloads
 *   need many concurrent sleepers.
 *
 * TODO(poll): replace the blocking nanosleep with a scheduler-integrated
 *   timer: the sleeping proc yields and registers a timer with the
 *   scheduler's poll loop; when the timer fires the proc is resumed.
 *   net / timer / io must all be unified under the SAME poll mechanism
 *   (one event loop owning timers + fds), not per-module ad-hoc waits.
 *   Tracked as the batch-3 timer task in docs/stdlib-port-plan.md.
 */

#define _POSIX_C_SOURCE 199309L /* nanosleep() under -std=c99 */

#include "ta.h"
#include <errno.h>
#include <time.h>

static Val time_now_ms(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)args;
    (void)nargs;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return val_int((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static Val time_monotonic_ms(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)args;
    (void)nargs;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return val_int((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static Val time_sleep(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_int(args[0]))
        return val_int(-1);
    int64_t ms = val_get_int(args[0]);
    if (ms < 0)
        ms = 0;
    struct timespec req;
    req.tv_sec = (time_t)(ms / 1000);
    req.tv_nsec = (long)(ms % 1000) * 1000000L;
    /* EINTR resumes with the remaining time in req. */
    while (nanosleep(&req, &req) == -1 && errno == EINTR) {
    }
    return val_int(0);
}

static TaFunc time_funcs[] = {{"now_ms", time_now_ms, 0},
                              {"monotonic_ms", time_monotonic_ms, 0},
                              {"sleep", time_sleep, 1},
                              {NULL, NULL, 0}};

/* Dynamic module entry: dlsym("vm_load_self") after dlopen (vm.c). */
void vm_load_self(VM *vm) { vm_register_module(vm, "time", time_funcs, 3); }