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
 * SLEEP MECHANISM (v2, poll-based — replaces the v1 blocking nanosleep):
 *   the calling proc arms its wait_deadline_ms (the SAME per-proc deadline
 *   field the net poll path already wakes on) and yields via vm_yield().
 *   OP_CCALL re-runs sleep from scratch on resume; the re-entry sees the
 *   armed deadline, and once now >= deadline it disarms and returns 0.
 *   The wake itself comes from the scheduler poll loop (worker loop in
 *   single-thread mode, the I/O poller thread otherwise), exactly like a
 *   net_connect deadline — net / timer / io / sleep all share ONE poll
 *   mechanism. No worker is ever blocked: concurrent sleeps cost one
 *   suspended proc each, not one thread each.
 *
 *   Semantics preserved from the v1 rationale in lib/time.ta: the
 *   mailbox is NOT touched (the proc is WAIT_IO, not WAIT_RECV, so
 *   mbox_deliver queues messages without waking or consuming), and the
 *   proc resumes no earlier than the full requested duration.
 *
 * TODO(poll): DONE — sleep now suspends through the unified poll
 *   mechanism described above.
 */

#define _POSIX_C_SOURCE 199309L /* clock_gettime() under -std=c99 */

#include "ta.h"
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

/* Poll-based sleep (see SLEEP MECHANISM in the header comment).
 *
 * OP_CCALL re-runs this callback from scratch after every wake, so the
 * two entries are told apart by wait_deadline_ms's own convention
 * (-1 = disarmed, shared with the net poll path, which always disarms
 * when its wait ends):
 *   disarmed  -> first entry: arm the deadline, watch nothing (no fd —
 *                the deadline alone does the waking), yield.
 *   armed     -> re-entry: expired? disarm and return 0. Otherwise a
 *                spurious wake — yield again without re-arming.
 * A returned nil means "suspended": OP_CCALL rewinds pc and the result
 * is discarded, same contract as net_connect's in-flight return. */
static Val time_sleep(VM *vm, Val *args, int nargs) {
    (void)nargs;
    if (!val_is_int(args[0]))
        return val_int(-1);
    int64_t ms = val_get_int(args[0]);
    if (ms <= 0)
        return val_int(0); /* nothing to wait for: no suspension */
    Proc *p = tls_current_proc;
    int64_t deadline = atomic_load(&p->wait_deadline_ms);
    if (deadline < 0) {
        atomic_store(&p->wait_deadline_ms, net_now_ms() + ms);
        p->wait_fd = -1; /* poll() ignores fd<0 entries; deadline-only wait */
        p->wait_events = 0;
        vm_wake_poller(vm); /* a blocked poll() must adopt the new deadline */
        vm_yield(vm);
        return val_nil();
    }
    if (net_now_ms() >= deadline) {
        atomic_store(&p->wait_deadline_ms, -1);
        return val_int(0);
    }
    vm_yield(vm);
    return val_nil();
}

static TaFunc time_funcs[] = {{"now_ms", time_now_ms, 0},
                              {"monotonic_ms", time_monotonic_ms, 0},
                              {"sleep", time_sleep, 1},
                              {NULL, NULL, 0}};

/* Dynamic module entry: dlsym("vm_load_self") after dlopen (vm.c). */
void vm_load_self(VM *vm) { vm_register_module(vm, "time", time_funcs, 3); }