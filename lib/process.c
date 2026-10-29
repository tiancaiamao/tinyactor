/*
 * lib/process.c — process module for TinyActor (built as lib/process.dylib / .so)
 *
 * Go os/exec-style child processes: spawn with stdin/stdout pipes, hand
 * the pipe fds to the caller (they go through the same net.read/net.write
 * + poller path as sockets), and reap on wait/close.
 *
 * The module is on the opaque Resource value (docs/c-resource-design.md):
 * no slot table, no generation handles, no lock. The child's bookkeeping
 * (pid + the two parent-side pipe fds) lives in a malloc'd ChildState
 * owned by the shared ResourcePayload; the actor-heap header (one per
 * holder, GC-moved) carries the ownership metadata. The TA side wraps
 * the payload value in the single-constructor ADT `type Proc { P(Resource) }`.
 *
 * Every operation runs the proc_open preamble: not owner → -2 (the Proc was
 * sent away — the pipes and the child belong to the receiver now, and this
 * stale copy must not look inside a payload the receiver may have freed),
 * then released → -1. The TA layer keeps
 *   the raw ints for the fd accessors and wait/close (callers test < 0,
 *   signatures unchanged); lib/process.ta lifts raw_run's -1 to
 *   Err("spawn failed").
 *
 * dtor = proc_dtor, in the four release contexts
 * (docs/c-resource-design.md §6.1 — a process dtor may block in
 * waitpid/kill, by design):
 *   - GC / proc_die / vm_free run it directly: the holder is dead or
 *     dropped its last reference, so nobody can ever read the pipes
 *     again — close them (child sees eof/SIGPIPE), SIGKILL the child
 *     and reap it. Prompt (bounded by the kill), zombie-free, and no
 *     TA allocation / VM entry.
 *   - explicit wait()/close() reap FIRST (graceful, no signal — the v1
 *     contract: close() waits for the child to exit on its own) and
 *     clear pid/fds before releasing, so the dtor then only frees the
 *     state.
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
 * The child's stderr is inherited, not piped (state is
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
 * Lifetime: wait() reaps the child, closes the remaining pipe fds and
 * releases the resource (later operations see Err "closed"). close() is
 * the not-yet-waited cleanup path: close both pipes + graceful reap,
 * idempotent for every alias of the header (release clears its payload
 * pointer). A stale copy — sent away to another actor — can do neither
 * (-2): it could otherwise close the receiver's pipes or reap the
 * receiver's child.
 *
 * Pipe fd ownership: the two parent-side fds belong to the MODULE. The
 * caller borrows them for net.read / net.write and must NOT
 * net.close() them — the module closes them at wait/close/dtor, and a
 * second close of an already-closed number can hit an unrelated fd
 * that reused it (another actor's pipe, say) and kill it. Signal eof
 * to the child's stdin with process.close_stdin(p) instead.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "ta.h"
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    pid_t pid;  /* child pid; -1 = reaped (wait/close already ran) */
    int in_fd;  /* parent's write end of the child's stdin pipe (-1 = closed) */
    int out_fd; /* parent's read end of the child's stdout pipe (-1 = closed) */
} ChildState;

static void proc_dtor(void *p) {
    ChildState *ps = (ChildState *)p;
    if (ps->in_fd >= 0)
        close(ps->in_fd);
    if (ps->out_fd >= 0)
        close(ps->out_fd);
    if (ps->pid != -1) {
        /* Nobody left to wait for this child (GC / proc_die / vm_free):
         * kill then reap — no signal semantics to preserve, and the kill
         * bounds the blocking waitpid. wait()/close() cleared pid first,
         * so their dtor pass lands here only on the free(). */
        kill(ps->pid, SIGKILL);
        waitpid(ps->pid, NULL, 0);
    }
    free(ps);
}

/* proc_open — resolve + validate a handle for one operation.
 * Returns 0 with the header handed out (state at r->res->ptr), or the
 * signal to relay: -1 invalid / released, -2 not owner.
 *
 * Ownership is checked before `res != NULL`: release frees the payload, and
 * a stale copy (sent away) can outlive it — only the owner may touch r->res. */
static int proc_open(Val v, HeapResource **out) {
    HeapResource *r = val_get_resource(v);
    if (r == NULL)
        return -1;
    if (r->owner_pid != tls_current_proc->pid)
        return -2;
    if (r->res == NULL)
        return -1;
    *out = r;
    return 0;
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
 * inherited. Returns the new resource value, or Int -1 on any failure
 * (the TA layer lifts by tag: int? -> Err("spawn failed")). The state
 * and its resource are created BEFORE the fork, so every failure path
 * just releases — the dtor sees sentinel-cleared fields and only frees
 * the state. */
static Val process_raw_run(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
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

    ChildState *ps = (ChildState *)malloc(sizeof(ChildState));
    if (!ps) {
        proc_free_argv(av);
        abort(); /* OOM: same treatment as every constructor */
    }
    ps->pid = -1;
    ps->in_fd = -1;
    ps->out_fd = -1;
    Val res = ta_resource_new(tls_current_proc, ps, proc_dtor);
    HeapResource *hr = val_get_resource(res);

    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) != 0) {
        ta_resource_release(hr);
        proc_free_argv(av);
        return val_int(-1);
    }
    if (pipe(out_pipe) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        ta_resource_release(hr);
        proc_free_argv(av);
        return val_int(-1);
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        ta_resource_release(hr);
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

    ps->pid = pid;
    ps->in_fd = in_pipe[1];
    ps->out_fd = out_pipe[0];
    proc_free_argv(av);
    return res;
}

static Val process_raw_stdin_w(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    HeapResource *r;
    int st = proc_open(args[0], &r);
    if (st < 0)
        return val_int(st);
    return val_int(((ChildState *)r->res->ptr)->in_fd);
}

static Val process_raw_stdout_r(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    HeapResource *r;
    int st = proc_open(args[0], &r);
    if (st < 0)
        return val_int(st);
    return val_int(((ChildState *)r->res->ptr)->out_fd);
}

/* Reap: close the remaining pipe fds (the child sees eof / SIGPIPE),
 * blocking-waitpid for the exit, and release the resource (later
 * operations see the released -1, matching v1's stale handle). Only a
 * normal exit yields its code (>= 0); -1 covers released handles,
 * waitpid errors, and abnormal termination. The blocking reap runs with
 * no lock held — a slow child cannot stall anyone else's spawns. */
static Val process_raw_wait(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    HeapResource *r;
    int st = proc_open(args[0], &r);
    if (st < 0)
        return val_int(st);
    ChildState *ps = (ChildState *)r->res->ptr;
    if (ps->pid == -1)
        return val_int(-1); /* already reaped */
    pid_t pid = ps->pid;
    ps->pid = -1;
    if (ps->in_fd >= 0) {
        close(ps->in_fd);
        ps->in_fd = -1;
    }
    if (ps->out_fd >= 0) {
        close(ps->out_fd);
        ps->out_fd = -1;
    }
    ta_resource_release(r); /* dtor sees sentinels: only frees the state */
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status))
        return val_int(-1);
    return val_int(WEXITSTATUS(status));
}

/* Not-yet-waited cleanup: close both pipes + graceful reap (no signal,
 * the child exits on its own — v1 contract), then release. Idempotent for
 * every alias of the handle (release clears the header's payload pointer);
 * a stale copy — owned by another actor — returns -2 and can never close
 * the receiver's pipes or reap the receiver's child. */
static Val process_raw_close(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    HeapResource *r = val_get_resource(args[0]);
    if (r == NULL)
        return val_int(-1);
    if (r->owner_pid != tls_current_proc->pid)
        return val_int(-2);
    if (r->res == NULL)
        return val_int(1); /* double close / wait-then-close */
    ChildState *ps = (ChildState *)r->res->ptr;
    pid_t pid = ps->pid;
    if (ps->in_fd >= 0) {
        close(ps->in_fd);
        ps->in_fd = -1;
    }
    if (ps->out_fd >= 0) {
        close(ps->out_fd);
        ps->out_fd = -1;
    }
    ps->pid = -1;
    ta_resource_release(r); /* frees the state */
    if (pid != -1) {
        int status = 0;
        waitpid(pid, &status, 0); /* blocks until the child exits on its own */
    }
    return val_int(1);
}

/* Close the parent's write end of the child's stdin pipe so the child
 * sees eof. The pipe fds belong to the module (see the header
 * comment), so this is the ONLY way to signal eof — a net.close() on
 * the borrowed fd would close a number the module still tracks.
 * Returns 1 on success, -1 when stdin is already closed / released. */
static Val process_raw_close_stdin(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    HeapResource *r;
    int st = proc_open(args[0], &r);
    if (st < 0)
        return val_int(st);
    ChildState *ps = (ChildState *)r->res->ptr;
    if (ps->in_fd < 0)
        return val_int(-1);
    int fd = ps->in_fd;
    ps->in_fd = -1;
    close(fd);
    return val_int(1);
}

static TaFunc process_funcs[] = {{"raw_run", process_raw_run, 2},
                                 {"raw_stdin_w", process_raw_stdin_w, 1},
                                 {"raw_stdout_r", process_raw_stdout_r, 1},
                                 {"raw_wait", process_raw_wait, 1},
                                 {"raw_close", process_raw_close, 1},
                                 {"raw_close_stdin", process_raw_close_stdin, 1},
                                 {NULL, NULL, 0}};

/* Dynamic module entry: dlsym("vm_load_self") after dlopen (main.c link_unit). */
void vm_load_self(VM *vm) { vm_register_module(vm, "process", process_funcs, 6); }