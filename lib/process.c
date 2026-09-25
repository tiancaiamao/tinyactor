/*
 * lib/process.c — process module for TinyActor (built as lib/process.dylib / .so)
 *
 * Go os/exec-style child processes: spawn with stdin/stdout pipes, hand
 * the pipe fds to the caller (they go through the same net.read/net.write
 * + poller path as sockets), and reap on wait/close.
 *
 * Storage layout follows lib/buffer.c's global table + int handle
 * pattern (slot | generation << SLOT_BITS, free list on close). The C
 * side works on BARE int handles; the TA side (lib/process.ta) wraps
 * them in the single-constructor ADT `type Proc { P(int) }`. Forgery is
 * prevented by the typechecker; the C side only validates handle
 * liveness.
 *
 *   raw_run(cmd, argv)       -> Int handle (>= 0), or -1 on any spawn
 *                               failure (bad types, table full, pipe /
 *                               fork / exec setup; TA lifts to
 *                               Err("spawn failed"))
 *   raw_stdin_w(h)           -> Int fd (write end of the child's stdin
 *                               pipe, parent side), or -1 (closed /
 *                               stale handle)
 *   raw_stdout_r(h)          -> Int fd (read end of the child's stdout
 *                               pipe, parent side), or -1 (closed /
 *                               stale handle)
 *   raw_wait(h)              -> Int exit code (>= 0, normal exit), or
 *                               -1 (closed / stale handle, waitpid
 *                               error, or abnormal termination)
 *   raw_close(h)             -> Int 1, idempotent for the SAME handle
 *                               value; -1 only for a stale handle
 *
 * Error convention (docs/c-module.md §4): -1 is the hard-error signal;
 * lib/process.ta lifts run()'s -1 to Err("spawn failed"). The fd /
 * exit-code accessors keep the raw -1 (their signatures are locked as
 * -> int in the plan; callers test < 0).
 *
 * Pipes and non-blocking (stdlib-port-plan Workstream C, v9): only the
 * TWO parent-held ends (write end of stdin, read end of stdout) get
 * O_NONBLOCK — a blocking read/write there would hang the worker and
 * break the net.c signal protocol (nil = suspended). The child's ends
 * must stay BLOCKING (an ordinary child would misread EAGAIN as an
 * error), but O_NONBLOCK is a file-status flag shared through dup2 via
 * the same open-file description. The ordering makes the window tiny:
 * the child dup2s and re-clears O_NONBLOCK on fd 0/1 immediately after
 * fork, while the parent sets O_NONBLOCK only on its own ends — which
 * are DIFFERENT open-file descriptions from the child's dup'd stdio, so
 * the parent's flip cannot reach the child. The child-side fcntl reset
 * is defense in depth for the fork race.
 *
 * The child's stderr is inherited, not piped (registry entry is
 * {pid, stdin_fd, stdout_fd} per the plan) — a piped stderr nobody
 * reads would just fill up and block the child.
 *
 * argv: raw_run(cmd, argv) execs `cmd` with argv[0] = cmd, followed by
 * the elements of the TA cons list `argv` (Go os/exec.Command shape:
 * the list does NOT repeat the program). `cmd` is a full path — there
 * is no PATH search (access(cmd, X_OK) is checked before fork so a
 * nonexistent command fails the spawn with Err instead of surfacing
 * later as a child exit 127; the check is TOCTOU-racy by nature, exec
 * still remains the authority). Every element must be a string;
 * anything else is the -1 hard error.
 *
 * Lifetime (docs/c-module.md §5): wait() reaps the child, closes the
 * remaining pipe fds, and removes the registry entry (the handle goes
 * stale). close() is the not-yet-waited cleanup path: close both pipes
 * + reap, idempotent for the same handle value. Neither sends signals —
 * close() blocks in waitpid until the child exits on its own (after its
 * stdout hits eof); use net.close(stdin_fd) to signal eof to a child
 * reading stdin. Stale handles (§5): closed slots go on a free list and
 * are recycled with a bumped generation, so an OLD handle value can
 * never alias a new process (any operation on it returns -1). A double
 * close of the SAME handle value is idempotent (Ok), like buffer.
 *
 * Leak surface (convention §5): a Proc whose owning actor dies without
 * wait/close leaks fds and an unreaped child until VM exit — v1
 * accepts this, same as buffer.
 *
 * Thread safety: none. The table is unprotected plain globals — same
 * data-race surface as lib/buffer.c today. Do not spawn from
 * concurrently running actors without external synchronization.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "ta.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define SLOT_BITS 8
#define MAX_PROC_SLOTS (1 << SLOT_BITS)

typedef struct {
    pid_t pid;  /* -1 = freed (on free list, awaiting reuse) */
    int in_fd;  /* parent's write end of the child's stdin pipe */
    int out_fd; /* parent's read end of the child's stdout pipe */
    int gen;
} ProcSlot;

static ProcSlot procs[MAX_PROC_SLOTS];
static int next_slot = 0;
static int free_head = -1; /* linked list of freed slot indices */
static int free_next[MAX_PROC_SLOTS];

/* Decode a handle to its slot, or NULL if invalid: out of range, a
 * generation that no longer matches (slot was closed and recycled), or
 * a freed slot still awaiting reuse (pid == -1). */
static ProcSlot *proc_get(int64_t h) {
    if (h < 0)
        return NULL;
    int slot = (int)(h & (MAX_PROC_SLOTS - 1));
    int gen = (int)(h >> SLOT_BITS);
    if (slot >= next_slot || procs[slot].gen != gen)
        return NULL;
    ProcSlot *p = &procs[slot];
    if (p->pid == -1)
        return NULL;
    return p;
}

/* Allocate a handle from the free list or the next fresh slot.
 * Recycled slots get a bumped generation so every handle minted before
 * the close decodes to NULL (stale) instead of aliasing the new
 * process. */
static int64_t proc_alloc_handle(void) {
    int slot;
    if (free_head >= 0) {
        slot = free_head;
        free_head = free_next[slot];
        free_next[slot] = -1;
        procs[slot].gen++;
    } else {
        if (next_slot >= MAX_PROC_SLOTS)
            return -1;
        slot = next_slot++;
        procs[slot].gen = 0;
    }
    return (int64_t)slot + ((int64_t)procs[slot].gen << SLOT_BITS);
}

/* Is a handle allocatable? raw_run checks this BEFORE forking so it
 * never spawns a child it cannot track; the actual allocation happens
 * right after a successful fork, where it cannot fail. */
static int proc_has_capacity(void) { return free_head >= 0 || next_slot < MAX_PROC_SLOTS; }

/* Close both parent-held pipe fds (if still open) and mark the slot
 * freed on the free list. The child must already be reaped (or is
 * reaped by the caller right after). */
static void proc_release_slot(int slot) {
    ProcSlot *p = &procs[slot];
    if (p->in_fd >= 0) {
        close(p->in_fd);
        p->in_fd = -1;
    }
    if (p->out_fd >= 0) {
        close(p->out_fd);
        p->out_fd = -1;
    }
    p->pid = -1;
    free_next[slot] = free_head;
    free_head = slot;
}

/* Build a NUL-terminated exec argv from cmd + a TA cons list of
 * strings: {cmd, elem..., NULL}. Returns NULL on bad types / OOM (the
 * caller has nothing to free on NULL). The copy is GC-invisible and
 * freed by the caller after fork. */
static char **proc_build_argv(VM *vm, Val cmd, Val argv) {
    (void)vm;
    if (!val_is_string(cmd))
        return NULL;
    HeapString *hs = val_get_string(cmd);

    int count = 0;
    Val node = argv;
    while (val_is_pair(node)) {
        if (!val_is_string(val_get_car(node)))
            return NULL;
        count++;
        node = val_get_cdr(node);
    }
    if (!val_is_nil(node))
        return NULL;

    char **av = malloc((size_t)(count + 2) * sizeof(char *));
    if (!av)
        return NULL;
    av[0] = malloc((size_t)hs->len + 1);
    if (!av[0]) {
        free(av);
        return NULL;
    }
    memcpy(av[0], hs->data, (size_t)hs->len);
    av[0][hs->len] = '\0';
    int i = 1;
    node = argv;
    while (val_is_pair(node)) {
        HeapString *e = val_get_string(val_get_car(node));
        av[i] = malloc((size_t)e->len + 1);
        if (!av[i]) { /* nothing smarter to do on OOM here */
            while (i > 0)
                free(av[--i]);
            free(av);
            return NULL;
        }
        memcpy(av[i], e->data, (size_t)e->len);
        av[i][e->len] = '\0';
        i++;
        node = val_get_cdr(node);
    }
    av[count + 1] = NULL;
    return av;
}

static void proc_free_argv(char **av) {
    for (int i = 0; av[i]; i++)
        free(av[i]);
    free(av);
}

/* Spawn cmd with argv (TA cons list of strings). stdin/stdout are
 * pipes (child ends blocking, parent ends O_NONBLOCK); stderr is
 * inherited. Returns the new handle, or -1 on any failure. */
static Val process_raw_run(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!proc_has_capacity())
        return val_int(-1);
    char **av = proc_build_argv(vm, args[0], args[1]);
    if (!av)
        return val_int(-1);
    const char *cmd = av[0];
    /* Fail fast on a missing / non-executable cmd — see the argv note
     * in the header comment (exec remains the authority; a cmd that
     * slips through still surfaces as a child exit 127 at wait). */
    if (access(cmd, X_OK) != 0) {
        proc_free_argv(av);
        return val_int(-1);
    }

    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) != 0) {
        proc_free_argv(av);
        return val_int(-1);
    }
    if (pipe(out_pipe) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        proc_free_argv(av);
        return val_int(-1);
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        proc_free_argv(av);
        return val_int(-1);
    }
    if (pid == 0) {
        /* child: wire the pipes onto stdio, force them BLOCKING (they
         * share their open-file description with the parent's ends —
         * see the header comment), then exec. After execv fails there
         * is nothing left to do but _exit; keep to async-signal-safe
         * calls (post-fork in a threaded program). */
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        fcntl(0, F_SETFL, 0);
        fcntl(1, F_SETFL, 0);
        execv(cmd, av);
        _exit(127);
    }

    /* parent: keep only the far ends, flip THEM non-blocking. These are
     * different open-file descriptions from the child's dup'd stdio, so
     * the child stays blocking (fork ordering: the child already dup2'd
     * + reset its own fds). */
    close(in_pipe[0]);
    close(out_pipe[1]);
    fcntl(in_pipe[1], F_SETFL, fcntl(in_pipe[1], F_GETFL) | O_NONBLOCK);
    fcntl(out_pipe[0], F_SETFL, fcntl(out_pipe[0], F_GETFL) | O_NONBLOCK);

    int64_t h = proc_alloc_handle(); /* cannot fail: capacity checked pre-fork */
    int slot = (int)(h & (MAX_PROC_SLOTS - 1));
    procs[slot].pid = pid;
    procs[slot].in_fd = in_pipe[1];
    procs[slot].out_fd = out_pipe[0];
    proc_free_argv(av);
    return val_int(h);
}

static Val process_raw_stdin_w(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    ProcSlot *p = proc_get(val_get_int(args[0]));
    if (!p)
        return val_int(-1);
    return val_int(p->in_fd);
}

static Val process_raw_stdout_r(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    ProcSlot *p = proc_get(val_get_int(args[0]));
    if (!p)
        return val_int(-1);
    return val_int(p->out_fd);
}

/* Reap: close the remaining pipe fds (the child sees eof / SIGPIPE),
 * blocking-waitpid for the exit, and remove the registry entry. Only a
 * normal exit yields its code (>= 0); -1 covers stale handles,
 * waitpid errors, and abnormal termination. */
static Val process_raw_wait(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    ProcSlot *p = proc_get(val_get_int(args[0]));
    if (!p)
        return val_int(-1);
    int slot = (int)(val_get_int(args[0]) & (MAX_PROC_SLOTS - 1));
    pid_t pid = p->pid;
    proc_release_slot(slot);
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status))
        return val_int(-1);
    return val_int(WEXITSTATUS(status));
}

/* Not-yet-waited cleanup: close both pipes + reap, idempotent for the
 * SAME handle value; a stale handle returns -1 and can never release
 * another process. */
static Val process_raw_close(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    int64_t h = val_get_int(args[0]);
    if (h < 0)
        return val_int(-1);
    int slot = (int)(h & (MAX_PROC_SLOTS - 1));
    int gen = (int)(h >> SLOT_BITS);
    if (slot >= next_slot || procs[slot].gen != gen)
        return val_int(-1);
    if (procs[slot].pid == -1)
        return val_int(1); /* double close of the same handle */
    pid_t pid = procs[slot].pid;
    proc_release_slot(slot);
    int status = 0;
    waitpid(pid, &status, 0);
    return val_int(1);
}

static TaFunc process_funcs[] = {
    {"raw_run", process_raw_run, 2},           {"raw_stdin_w", process_raw_stdin_w, 1},
    {"raw_stdout_r", process_raw_stdout_r, 1}, {"raw_wait", process_raw_wait, 1},
    {"raw_close", process_raw_close, 1},       {NULL, NULL, 0}};

/* Dynamic module entry: dlsym("vm_load_self") after dlopen (vm.c). */
void vm_load_self(VM *vm) { vm_register_module(vm, "process", process_funcs, 5); }