/*
 * timer.c — Erlang-style timers, driven by the scheduler's poll loop
 *
 * The timer table hangs off the SAME poll mechanism as net I/O: the
 * scheduler (io_poller_thread in multi-thread mode, the worker loop in
 * single-thread mode) clamps its poll() timeout to
 * timer_next_deadline_ms() and calls timer_fire_expired() on every tick.
 * There is no separate timer thread and no second event loop — see the
 * poll-unification requirement in docs/stdlib-port-plan.md (batch 3).
 *
 *   timer.send_after(pid, ms, msg)    one-shot: deliver msg to pid after
 *                                     ms; returns a timer id (> 0)
 *   timer.send_interval(pid, ms, msg) periodic: deliver every ms until
 *                                     cancelled; returns a timer id
 *   timer.cancel(id)                  1 if a pending timer was cancelled,
 *                                     0 if the id already fired / is
 *                                     unknown (same convention as Erlang:
 *                                     cancelling is always safe)
 *
 * Data structure: a singly-linked list kept sorted by deadline. Timers
 * registered by an actor run are few, and the two hot operations the
 * scheduler performs per tick — earliest-deadline peek and due-fire
 * pop — are O(1) on a sorted list. Cancel / insert are O(n) with tiny
 * constants; a heap would buy nothing at this scale but cost the
 * sift-up/down code.
 *
 * Message capture: at registration the message tree is snapshotted into
 * a malloc'd MsgFragment (frag_calc_size/frag_copy — plain malloc, GC
 * invisible), so the timer outlives the caller's heap collections and
 * even the caller itself. On fire the snapshot is delivered through
 * mbox_deliver, the same path as a regular send (a dead target simply
 * drops the message).
 *
 * TimerState is process-lifetime, per-VM, lazily registered — same
 * convention as net.c's NetState registry (tavm runs one VM per process).
 */

#include "ta.h"
#include <pthread.h>
#include <stdlib.h>

typedef struct Timer {
    int64_t id;
    int target_pid;
    int64_t interval_ms; /* 0 = one-shot (send_after), > 0 = interval */
    int64_t deadline_ms; /* next fire time, monotonic ms */
    MsgFragment *frag;   /* malloc'd message snapshot (GC-invisible) */
    struct Timer *next;  /* sorted by deadline_ms ascending */
} Timer;

typedef struct TimerState {
    VM *vm;
    pthread_mutex_t lock;
    Timer *head;     /* sorted by deadline_ms ascending, NULL = none */
    int64_t next_id; /* ids count from 1; 0 is the "invalid" id */
    struct TimerState *next_state;
} TimerState;

static TimerState *g_timer_states = NULL;
static pthread_mutex_t g_timer_lock = PTHREAD_MUTEX_INITIALIZER;

/* Look up the per-VM TimerState, or NULL when none was ever created
 * (the scheduler's per-tick driver calls must not allocate for VMs
 * that never use timers). Creating callers pass create=1. */
static TimerState *find_timer_state(VM *vm) {
    pthread_mutex_lock(&g_timer_lock);
    for (TimerState *ts = g_timer_states; ts; ts = ts->next_state) {
        if (ts->vm == vm) {
            pthread_mutex_unlock(&g_timer_lock);
            return ts;
        }
    }
    pthread_mutex_unlock(&g_timer_lock);
    return NULL;
}

static TimerState *get_timer_state(VM *vm) {
    TimerState *ts = find_timer_state(vm);
    if (ts)
        return ts;
    ts = calloc(1, sizeof(TimerState));
    if (!ts)
        return NULL;
    ts->vm = vm;
    pthread_mutex_init(&ts->lock, NULL);
    pthread_mutex_lock(&g_timer_lock);
    /* Re-check: a concurrent registrant may have created it first. The
     * duplicate (if any) is never used again and, like every TimerState,
     * reclaimed at process exit. */
    for (TimerState *e = g_timer_states; e; e = e->next_state) {
        if (e->vm == vm) {
            pthread_mutex_unlock(&g_timer_lock);
            free(ts);
            return e;
        }
    }
    ts->next_state = g_timer_states;
    g_timer_states = ts;
    pthread_mutex_unlock(&g_timer_lock);
    return ts;
}

/* Sorted insert; caller holds ts->lock. */
static void timer_insert(TimerState *ts, Timer *t) {
    Timer **pp = &ts->head;
    while (*pp && (*pp)->deadline_ms <= t->deadline_ms)
        pp = &(*pp)->next;
    t->next = *pp;
    *pp = t;
}

static int64_t timer_register(VM *vm, int target_pid, int64_t ms, int64_t interval_ms, Val msg) {
    TimerState *ts = get_timer_state(vm);
    if (!ts)
        return 0;
    /* Snapshot the message before it can be collected with the caller's
     * heap: this is plain malloc outside any fromspace (see the fragment
     * comment in scheduler.c). */
    MsgFragment *frag = malloc(sizeof(MsgFragment) + frag_calc_size(msg));
    if (!frag)
        return 0;
    frag->next = NULL;
    frag->size = 0;
    frag->root = frag_copy(frag, msg);
    Timer *t = calloc(1, sizeof(Timer));
    if (!t) {
        free(frag);
        return 0;
    }
    pthread_mutex_lock(&ts->lock);
    t->id = ++ts->next_id;
    t->target_pid = target_pid;
    t->interval_ms = interval_ms;
    t->deadline_ms = net_now_ms() + ms;
    t->frag = frag;
    timer_insert(ts, t);
    pthread_mutex_unlock(&ts->lock);
    /* A poll() already blocked on the lazy 100ms cap must re-scan and
     * adopt the new (earlier) deadline. */
    vm_wake_poller(vm);
    return t->id;
}

int64_t timer_next_deadline_ms(VM *vm) {
    TimerState *ts = find_timer_state(vm);
    if (!ts)
        return -1;
    pthread_mutex_lock(&ts->lock);
    int64_t d = ts->head ? ts->head->deadline_ms : -1;
    pthread_mutex_unlock(&ts->lock);
    return d;
}

void timer_fire_expired(VM *vm, int64_t now_ms) {
    TimerState *ts = find_timer_state(vm);
    if (!ts)
        return;
    for (;;) {
        pthread_mutex_lock(&ts->lock);
        Timer *t = ts->head;
        if (!t || t->deadline_ms > now_ms) {
            pthread_mutex_unlock(&ts->lock);
            return;
        }
        ts->head = t->next;
        pthread_mutex_unlock(&ts->lock);

        /* Deliver outside the timer lock: mbox_deliver takes the target's
         * mbox_lock, and holding ts->lock across it would create a
         * second lock order (timer -> mbox) next to none elsewhere.
         * Same target lookup/bounding as b_send (src/builtin.c);
         * mbox_deliver itself drops the message for a dead target. */
        Proc *target =
            (t->target_pid >= 0 && t->target_pid < vm->procs_cap) ? vm->procs[t->target_pid] : NULL;
        if (target && atomic_load(&target->state) != PROC_DEAD)
            mbox_deliver(vm, target, t->frag->root);

        if (t->interval_ms > 0) {
            /* Periodic: re-arm from `now` (not from the old deadline) so
             * a slow tick does not fire a burst of catch-up deliveries. */
            pthread_mutex_lock(&ts->lock);
            t->deadline_ms = now_ms + t->interval_ms;
            timer_insert(ts, t);
            pthread_mutex_unlock(&ts->lock);
        } else {
            free(t->frag);
            free(t);
        }
    }
}

/* ============================================================
 * C functions — statically registered ("timer" module, like net)
 * ============================================================ */

static Val timer_c_send_after(VM *vm, Val *args, int nargs) {
    (void)nargs;
    if (!val_is_int(args[1]))
        return val_int(0);
    int64_t ms = val_get_int(args[1]);
    if (ms < 0)
        ms = 0;
    return val_int(timer_register(vm, (int)val_get_pid(args[0]), ms, 0, args[2]));
}

static Val timer_c_send_interval(VM *vm, Val *args, int nargs) {
    (void)nargs;
    if (!val_is_int(args[1]))
        return val_int(0);
    int64_t ms = val_get_int(args[1]);
    if (ms < 0)
        ms = 0;
    return val_int(timer_register(vm, (int)val_get_pid(args[0]), ms, ms, args[2]));
}

static Val timer_c_cancel(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_int(args[0]))
        return val_int(0);
    int64_t id = val_get_int(args[0]);
    if (id <= 0)
        return val_int(0);
    TimerState *ts = find_timer_state(vm);
    if (!ts)
        return val_int(0);
    pthread_mutex_lock(&ts->lock);
    for (Timer **pp = &ts->head; *pp; pp = &(*pp)->next) {
        if ((*pp)->id == id) {
            Timer *t = *pp;
            *pp = t->next;
            pthread_mutex_unlock(&ts->lock);
            free(t->frag);
            free(t);
            return val_int(1);
        }
    }
    pthread_mutex_unlock(&ts->lock);
    return val_int(0);
}

static TaFunc timer_funcs[] = {{"send_after", timer_c_send_after, 3},
                               {"send_interval", timer_c_send_interval, 3},
                               {"cancel", timer_c_cancel, 1},
                               {NULL, NULL, 0}};

void vm_register_timer_module(VM *vm) { vm_register_module(vm, "timer", timer_funcs, 3); }