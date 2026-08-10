/*
 * net.c — Network C module for TinyActor VM
 *
 * Non-blocking TCP sockets: listen, accept, read, write, close.
 * On EAGAIN/EWOULDBLOCK, calls vm_watch_fd() + vm_yield() to
 * suspend the current actor cleanly (no magic return value).
 *
 * net_connect (issue #29) is fully non-blocking end-to-end and returns
 * distinguishable error symbols:
 *
 *   stage 1 — DNS: numeric-IP fast path (inet_pton), otherwise a per-VM
 *             single resolver thread resolves the hostname and wakes the
 *             actor through a wake pipe (vm_watch_fd + vm_yield). The
 *             overall connect deadline (timeout_ms) applies to this phase
 *             too, so a hung getaddrinfo returns 'timeout instead of
 *             suspending the actor indefinitely.
 *   stage 2 — non-blocking connect(): EINPROGRESS registers the socket in
 *             a per-VM connects table (keyed by proc pid) and waits on
 *             POLLOUT; the I/O poller also wakes the actor at its deadline.
 *             Stage 2 inherits the stage-1 overall deadline (remaining
 *             budget), so DNS + connect together never exceed timeout_ms
 *             by more than one poll cycle.
 *   stage 3 — completion: on re-entry the connects table entry decides —
 *             deadline passed -> 'timeout, SO_ERROR==0 -> fd, else
 *             'refused / 'error.
 *
 * Return contract:
 *   success  -> fd (int, as before)
 *   in flight-> nil (the actor is suspended; TA retries like net_read)
 *   failure  -> 'dns_error (getaddrinfo EAI_*) / 'refused (ECONNREFUSED)
 *               / 'timeout (deadline exceeded) / 'error (other errno)
 */

/*
 * Linux requires _POSIX_C_SOURCE for clock_gettime/CLOCK_MONOTONIC,
 * getaddrinfo and struct addrinfo under -std=c99; macOS exposes them by
 * default, which is why the build only breaks on Linux. On Apple, keep
 * BSD constants (e.g. INADDR_LOOPBACK) visible that _POSIX_C_SOURCE
 * would otherwise hide. Must come before any #include.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#include "ta.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define NET_CONNECT_DEFAULT_TIMEOUT_MS 10000

/* ============================================================
 * Per-VM connect/DNS registry
 *
 * The C functions themselves are stateless; progress across yields is
 * tracked in a per-VM registry keyed by proc pid (an actor runs
 * sequentially, so at most one connect is in flight per pid). The
 * registry is process-lifetime: tavm runs one VM per process and the
 * resolver thread outlives vm_run, so entries are reclaimed by process
 * exit rather than explicit teardown (see tavm.c main).
 * ============================================================ */

typedef struct NetResolve {
    int pid;
    char *host; /* strdup'd, owned by this entry */
    int port;
    int pipe_r, pipe_w;  /* wake pipe: resolver writes, worker polls read end */
    int state;           /* 0=queued 1=in-progress 2=done */
    int cancelled;       /* 1 = the worker timed out while getaddrinfo was
                            running; the resolver owns the entry and must
                            free it (result + struct) when it returns */
    int64_t deadline_ms; /* overall connect deadline, DNS phase included */
    int eai_err;         /* getaddrinfo EAI_* when done+failed, else 0 */
    struct addrinfo *ai; /* getaddrinfo result when done+ok; worker frees */
    struct NetResolve *next;
} NetResolve;

typedef struct NetConnect {
    int pid;
    int fd;
    int64_t deadline_ms; /* monotonic ms */
    struct NetConnect *next;
} NetConnect;

typedef struct NetState {
    VM *vm;
    pthread_mutex_t lock;
    pthread_cond_t resolve_cond;
    pthread_t resolver;
    int resolver_started;
    NetResolve *resolve_queue; /* queued DNS requests (FIFO) */
    NetResolve *resolve_tail;
    NetResolve *in_flight; /* dequeued by the resolver, getaddrinfo running */
    NetResolve *resolves;  /* done requests awaiting worker pickup */
    NetConnect *connects;  /* in-progress non-blocking connects */
    struct NetState *next;
} NetState;

static NetState *g_net_states = NULL;
static pthread_mutex_t g_net_lock = PTHREAD_MUTEX_INITIALIZER;

int64_t net_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static Val net_sym(VM *vm, const char *name) {
    int idx = vm_intern_symbol(vm, name);
    return val_symbol((uint32_t)idx);
}

/* Look up (or lazily create) the per-VM NetState. Never freed — see the
 * registry comment above. */
static NetState *get_net_state(VM *vm) {
    pthread_mutex_lock(&g_net_lock);
    for (NetState *ns = g_net_states; ns; ns = ns->next) {
        if (ns->vm == vm) {
            pthread_mutex_unlock(&g_net_lock);
            return ns;
        }
    }
    NetState *ns = calloc(1, sizeof(NetState));
    if (ns) {
        ns->vm = vm;
        pthread_mutex_init(&ns->lock, NULL);
        pthread_cond_init(&ns->resolve_cond, NULL);
        ns->next = g_net_states;
        g_net_states = ns;
    }
    pthread_mutex_unlock(&g_net_lock);
    return ns;
}

/* ============================================================
 * Resolver thread — one per VM, created lazily on the first hostname
 * connect. Resolves queued hostnames with blocking getaddrinfo() on its
 * own thread (workers never block), then hands the result to the worker
 * through the resolves list + a byte on the request's wake pipe.
 * ============================================================ */

static void *net_resolver_thread(void *arg) {
    NetState *ns = (NetState *)arg;
    /* A worker may die while its DNS request is in flight, closing the
     * pipe read end; writing to a readerless pipe raises SIGPIPE. Ignore
     * it (standard for network programs) so the write simply fails with
     * EPIPE instead of killing the process. */
    signal(SIGPIPE, SIG_IGN);
    for (;;) {
        pthread_mutex_lock(&ns->lock);
        while (!ns->resolve_queue)
            pthread_cond_wait(&ns->resolve_cond, &ns->lock);
        NetResolve *r = ns->resolve_queue;
        ns->resolve_queue = r->next;
        if (!ns->resolve_queue)
            ns->resolve_tail = NULL;
        r->next = NULL;
        r->state = 1; /* in-progress; from here on the resolver owns r */
        r->next = ns->in_flight;
        ns->in_flight = r;
        pthread_mutex_unlock(&ns->lock);

        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET; /* IPv4 only (issue #29 scope) */
        hints.ai_socktype = SOCK_STREAM;
        char portstr[16];
        snprintf(portstr, sizeof(portstr), "%d", r->port);
        r->eai_err = getaddrinfo(r->host, portstr, &hints, &r->ai);

        pthread_mutex_lock(&ns->lock);
        /* Unlink r from in_flight (only the resolver removes entries). */
        NetResolve **pp = &ns->in_flight;
        while (*pp) {
            if (*pp == r) {
                *pp = r->next;
                break;
            }
            pp = &(*pp)->next;
        }
        r->next = NULL;
        if (r->cancelled) {
            /* The worker timed out while getaddrinfo was running and
             * closed the pipes; the worker must not free this entry.
             * Drop the result and the entry here. */
            pthread_mutex_unlock(&ns->lock);
            if (r->ai)
                freeaddrinfo(r->ai);
            free(r->host);
            free(r);
            continue; /* skip the pipe write */
        }
        r->state = 2; /* done */
        r->next = ns->resolves;
        ns->resolves = r;
        pthread_mutex_unlock(&ns->lock);

        char b = 'R';
        if (write(r->pipe_w, &b, 1) < 0) {
            /* Reader gone (proc died): the entry leaks until process exit
             * (registry is process-lifetime) and pipe_w is closed then. */
        }
    }
    return NULL;
}

/* Caller must hold ns->lock. */
static NetResolve *net_resolve_find(NetState *ns, int pid) {
    for (NetResolve *r = ns->resolves; r; r = r->next)
        if (r->pid == pid)
            return r;
    return NULL;
}

/* Caller must hold ns->lock. */
static void net_resolve_remove(NetState *ns, NetResolve *target) {
    NetResolve **pp = &ns->resolves;
    while (*pp) {
        if (*pp == target) {
            *pp = target->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

/* Caller must hold ns->lock. Returns the not-yet-done DNS entry for pid
 * (still queued, or already dequeued and being resolved), or NULL. Sets
 * *inflight to 1 when the resolver owns the entry (getaddrinfo running),
 * 0 when it is still in the FIFO queue and the worker owns it. */
static NetResolve *net_resolve_find_pending(NetState *ns, int pid, int *inflight) {
    for (NetResolve *r = ns->resolve_queue; r; r = r->next)
        if (r->pid == pid) {
            *inflight = 0;
            return r;
        }
    for (NetResolve *r = ns->in_flight; r; r = r->next)
        if (r->pid == pid) {
            *inflight = 1;
            return r;
        }
    return NULL;
}

/* Caller must hold ns->lock. Unlink r (which must be in resolve_queue)
 * and fix the tail pointer. */
static void net_resolve_unlink_queue(NetState *ns, NetResolve *r) {
    NetResolve **pp = &ns->resolve_queue;
    while (*pp) {
        if (*pp == r) {
            *pp = r->next;
            break;
        }
        pp = &(*pp)->next;
    }
    r->next = NULL;
    if (ns->resolve_tail == r) {
        NetResolve *t = ns->resolve_queue;
        while (t && t->next)
            t = t->next;
        ns->resolve_tail = t;
    }
}

/* ============================================================
 * Stage 3 — completion of a non-blocking connect (POLLOUT or deadline
 * wake re-entry). Consumes the connects-table entry on every terminal
 * path (success, timeout, refused, error) so nothing leaks.
 * ============================================================ */

static Val net_connect_finish(VM *vm, int pid, NetState *ns, int *handled) {
    Proc *p = tls_current_proc;
    *handled = 0;
    pthread_mutex_lock(&ns->lock);
    NetConnect **pp = &ns->connects;
    while (*pp) {
        NetConnect *e = *pp;
        if (e->pid == pid) {
            *handled = 1;
            if (net_now_ms() > e->deadline_ms) {
                *pp = e->next;
                pthread_mutex_unlock(&ns->lock);
                close(e->fd);
                free(e);
                p->wait_deadline_ms = -1;
                return net_sym(vm, "timeout");
            }
            int err = 0;
            socklen_t len = sizeof(err);
            if (getsockopt(e->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
                err = errno;
            if (err == 0) {
                *pp = e->next;
                int fd = e->fd;
                pthread_mutex_unlock(&ns->lock);
                free(e);
                p->wait_deadline_ms = -1;
                return val_int(fd);
            }
            if (err == ECONNREFUSED) {
                *pp = e->next;
                pthread_mutex_unlock(&ns->lock);
                close(e->fd);
                free(e);
                p->wait_deadline_ms = -1;
                return net_sym(vm, "refused");
            }
            if (err == EINPROGRESS || err == EALREADY) {
                /* Spurious POLLOUT before the handshake finished — keep
                 * waiting on the same socket and deadline. */
                pthread_mutex_unlock(&ns->lock);
                vm_watch_fd(vm, e->fd, POLLOUT);
                p->wait_deadline_ms = e->deadline_ms;
                vm_yield(vm);
                return val_nil();
            }
            /* Any other error (ECONNRESET, ETIMEDOUT, ENETUNREACH, ...) */
            *pp = e->next;
            pthread_mutex_unlock(&ns->lock);
            close(e->fd);
            free(e);
            p->wait_deadline_ms = -1;
            return net_sym(vm, "error");
        }
        pp = &e->next;
    }
    pthread_mutex_unlock(&ns->lock);
    return val_nil();
}

/* ============================================================
 * Stage 2 — non-blocking connect() against a resolved sockaddr.
 *
 * overall_deadline_ms is the absolute monotonic deadline for the whole
 * connect operation (DNS phase included). For the numeric-IP fast path
 * the caller passes now + timeout_ms; after DNS it passes the done
 * entry's r->deadline_ms, i.e. the remaining budget, so the connect
 * phase does NOT restart the clock — DNS + connect together stay within
 * the original timeout_ms (+ one poll cycle).
 * ============================================================ */

static Val net_connect_sockaddr(VM *vm, int pid, const struct sockaddr *sa, socklen_t salen,
                                int64_t overall_deadline_ms, NetState *ns) {
    Proc *p = tls_current_proc;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return net_sym(vm, "error");
    set_nonblocking(fd);

    if (connect(fd, sa, salen) == 0)
        return val_int(fd); /* connected immediately (loopback) */

    if (errno == EINPROGRESS || errno == EALREADY) {
        NetConnect *e = malloc(sizeof(NetConnect));
        if (!e) {
            close(fd);
            return net_sym(vm, "error");
        }
        e->pid = pid;
        e->fd = fd;
        e->deadline_ms = overall_deadline_ms;
        pthread_mutex_lock(&ns->lock);
        e->next = ns->connects;
        ns->connects = e;
        pthread_mutex_unlock(&ns->lock);
        vm_watch_fd(vm, fd, POLLOUT);
        p->wait_deadline_ms = e->deadline_ms;
        vm_yield(vm);
        return val_nil();
    }

    if (errno == EISCONN)
        return val_int(fd); /* already connected */

    int e = errno;
    close(fd);
    if (e == ECONNREFUSED)
        return net_sym(vm, "refused");
    return net_sym(vm, "error");
}

/* ============================================================
 * Stage 1 (slow path) — enqueue a DNS request and wait on its wake pipe.
 * ============================================================ */

static Val net_connect_dns_enqueue(VM *vm, int pid, const char *host, int port, int64_t timeout_ms,
                                   NetState *ns) {
    Proc *p = tls_current_proc;
    NetResolve *r = calloc(1, sizeof(NetResolve));
    if (!r)
        return net_sym(vm, "error");
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        free(r);
        return net_sym(vm, "error");
    }
    r->pid = pid;
    r->host = strdup(host);
    r->port = port;
    r->pipe_r = pipefd[0];
    r->pipe_w = pipefd[1];
    r->state = 0;
    r->cancelled = 0;
    /* The deadline covers the whole connect, DNS included: a hung
     * getaddrinfo must not suspend the actor past timeout_ms. */
    r->deadline_ms = net_now_ms() + timeout_ms;
    if (!r->host) {
        close(pipefd[0]);
        close(pipefd[1]);
        free(r);
        return net_sym(vm, "error");
    }

    pthread_mutex_lock(&ns->lock);
    if (ns->resolve_tail)
        ns->resolve_tail->next = r;
    else
        ns->resolve_queue = r;
    ns->resolve_tail = r;
    if (!ns->resolver_started) {
        ns->resolver_started = 1;
        if (pthread_create(&ns->resolver, NULL, net_resolver_thread, ns) != 0) {
            ns->resolver_started = 0;
            /* roll back the enqueue */
            if (ns->resolve_queue == r) {
                ns->resolve_queue = r->next;
                if (!ns->resolve_queue)
                    ns->resolve_tail = NULL;
            } else {
                NetResolve *prev = ns->resolve_queue;
                while (prev && prev->next != r)
                    prev = prev->next;
                if (prev)
                    prev->next = r->next;
                if (ns->resolve_tail == r)
                    ns->resolve_tail = prev;
            }
            pthread_mutex_unlock(&ns->lock);
            close(pipefd[0]);
            close(pipefd[1]);
            free(r->host);
            free(r);
            return net_sym(vm, "error");
        }
    }
    pthread_cond_signal(&ns->resolve_cond);
    pthread_mutex_unlock(&ns->lock);

    vm_watch_fd(vm, pipefd[0], POLLIN);
    /* Deadline wake during DNS: bounds the whole connect, not just the
     * TCP handshake, so a hung getaddrinfo cannot suspend this actor
     * forever (or stall every later hostname connect behind it). */
    p->wait_deadline_ms = r->deadline_ms;
    vm_yield(vm);
    return val_nil();
}

/* ============================================================
 * net_connect — three-stage non-blocking connect.
 * ============================================================ */

static Val net_connect(VM *vm, Val *args, int nargs) {
    Proc *p = tls_current_proc;
    int pid = p->pid;
    NetState *ns = get_net_state(vm);
    if (!ns)
        return val_int(-1);

    /* Parse args. On a yield re-entry the VM replays the same args, so
     * host/port/timeout are identical to the original call. */
    if (nargs < 2 || !val_is_string(args[0]) || !val_is_int(args[1]))
        return val_int(-1);
    HeapString *host = val_get_string(args[0]);
    int port = (int)val_get_int(args[1]);
    int64_t timeout_ms = NET_CONNECT_DEFAULT_TIMEOUT_MS;
    if (nargs >= 3 && val_is_int(args[2])) {
        int64_t t = val_get_int(args[2]);
        if (t > 0)
            timeout_ms = t;
    }

    /* Clear any stale deadline from a previous wait (fresh entry point). */
    p->wait_deadline_ms = -1;

    /* ---- Stage 1 re-entry: a DNS resolution for this pid finished or
     * timed out? ---- */
    pthread_mutex_lock(&ns->lock);
    NetResolve *r = net_resolve_find(ns, pid); /* done list */
    int64_t now = net_now_ms();
    if (r) {
        if (now > r->deadline_ms) {
            /* The DNS finished, but the overall deadline has already
             * passed — treat the whole connect as timed out, matching the
             * stage-3 '> deadline' rule. */
            struct addrinfo *ai = r->ai;
            int pipe_r = r->pipe_r, pipe_w = r->pipe_w;
            net_resolve_remove(ns, r);
            free(r->host);
            free(r);
            pthread_mutex_unlock(&ns->lock);
            if (pipe_r >= 0)
                close(pipe_r);
            if (pipe_w >= 0)
                close(pipe_w);
            if (p->wait_fd == pipe_r)
                p->wait_fd = -1;
            if (ai)
                freeaddrinfo(ai);
            p->wait_deadline_ms = -1;
            return net_sym(vm, "timeout");
        }
        int eai = r->eai_err;
        struct addrinfo *ai = r->ai;
        int pipe_r = r->pipe_r, pipe_w = r->pipe_w;
        /* The overall deadline (enqueue + timeout_ms) is the remaining
         * budget for stage 2: pass it through so the connect phase does
         * not restart the clock with a fresh timeout_ms. */
        int64_t overall_deadline_ms = r->deadline_ms;
        net_resolve_remove(ns, r);
        free(r->host);
        free(r);
        pthread_mutex_unlock(&ns->lock);

        if (pipe_r >= 0)
            close(pipe_r);
        if (pipe_w >= 0)
            close(pipe_w);
        if (p->wait_fd == pipe_r)
            p->wait_fd = -1;

        if (eai != 0) {
            if (ai)
                freeaddrinfo(ai);
            return net_sym(vm, "dns_error");
        }
        Val v = net_connect_sockaddr(vm, pid, ai->ai_addr, ai->ai_addrlen, overall_deadline_ms, ns);
        freeaddrinfo(ai);
        return v;
    }

    /* Still pending: either queued (the resolver has not dequeued it yet)
     * or in-flight (the resolver is inside getaddrinfo). If the deadline
     * passed, cancel the request and return 'timeout; otherwise the wake
     * was spurious and we keep waiting on the wake pipe. */
    int inflight = 0;
    NetResolve *rp = net_resolve_find_pending(ns, pid, &inflight);
    if (rp) {
        if (now >= rp->deadline_ms) {
            int pipe_r = rp->pipe_r, pipe_w = rp->pipe_w;
            if (inflight) {
                /* The resolver owns the entry — flag it so it frees the
                 * result and the struct when getaddrinfo returns; we only
                 * close the pipes (worker side). */
                rp->cancelled = 1;
            } else {
                /* Still queued and never dequeued — the worker owns it,
                 * so unlink and free it here. */
                net_resolve_unlink_queue(ns, rp);
                free(rp->host);
                free(rp);
            }
            pthread_mutex_unlock(&ns->lock);
            if (pipe_r >= 0)
                close(pipe_r);
            if (pipe_w >= 0)
                close(pipe_w);
            if (p->wait_fd == pipe_r)
                p->wait_fd = -1;
            p->wait_deadline_ms = -1;
            return net_sym(vm, "timeout");
        }
        /* Within the deadline: re-arm the pipe wait and yield again. This
         * also guards against falling through to the fresh-call path and
         * double-enqueuing the same pid. */
        int pipe_r = rp->pipe_r;
        int64_t deadline_ms = rp->deadline_ms;
        pthread_mutex_unlock(&ns->lock);
        vm_watch_fd(vm, pipe_r, POLLIN);
        p->wait_deadline_ms = deadline_ms;
        vm_yield(vm);
        return val_nil();
    }
    pthread_mutex_unlock(&ns->lock);

    /* ---- Stage 3 re-entry: an in-progress non-blocking connect? ---- */
    {
        int handled = 0;
        Val v = net_connect_finish(vm, pid, ns, &handled);
        if (handled)
            return v;
    }

    /* ---- Fresh call ---- */
    /* Fast path: numeric IPv4 — covers every existing caller
     * (e.g. "127.0.0.1") with zero extra cost. */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host->data, &addr.sin_addr) == 1) {
        return net_connect_sockaddr(vm, pid, (struct sockaddr *)&addr, sizeof(addr),
                                    net_now_ms() + timeout_ms, ns);
    }

    /* Slow path: enqueue DNS resolution and wait on the wake pipe. */
    return net_connect_dns_enqueue(vm, pid, host->data, port, timeout_ms, ns);
}

static Val net_close(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    int fd = (int)val_get_int(args[0]);
    close(fd);
    return val_nil();
}

static Val sym_eof(VM *vm) {
    int idx = vm_intern_symbol(vm, "eof");
    return val_symbol((uint32_t)idx);
}

static Val net_listen(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    int port = (int)val_get_int(args[0]);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return val_int(-1);

    int optval = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 512) < 0) {
        close(fd);
        return val_int(-1);
    }

    set_nonblocking(fd);
    return val_int(fd);
}

static Val net_accept(VM *vm, Val *args, int nargs) {
    (void)nargs;
    int server_fd = (int)val_get_int(args[0]);

    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            vm_watch_fd(vm, server_fd, POLLIN);
            vm_yield(vm);
            return val_nil();
        }
        return val_int(-1);
    }

    set_nonblocking(client_fd);
    return val_int(client_fd);
}

static Val net_read(VM *vm, Val *args, int nargs) {
    Proc *p = tls_current_proc;
    int fd = (int)val_get_int(args[0]);
    int max_len = 4096;
    if (nargs >= 2)
        max_len = (int)val_get_int(args[1]);
    if (max_len <= 0)
        max_len = 4096;
    if (max_len > 65536)
        max_len = 65536;

    char *buf = malloc((size_t)max_len);
    if (!buf)
        return val_int(-1);
    ssize_t n = read(fd, buf, (size_t)max_len);
    if (n < 0) {
        free(buf);
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            vm_watch_fd(vm, fd, POLLIN);
            vm_yield(vm);
            return val_nil();
        }
        return val_int(-1);
    }
    if (n == 0) {
        free(buf);
        return sym_eof(vm);
    }

    Val result = val_string(p, buf, (int)n);
    free(buf);
    return result;
}

static Val net_write(VM *vm, Val *args, int nargs) {
    (void)nargs;
    int fd = (int)val_get_int(args[0]);
    if (!val_is_string(args[1]))
        return val_int(-1);

    HeapString *hs = val_get_string(args[1]);
    ssize_t n = write(fd, hs->data, (size_t)hs->len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            vm_watch_fd(vm, fd, POLLOUT);
            vm_yield(vm);
            return val_nil();
        }
        return val_int(-1);
    }
    return val_int((int64_t)n);
}

TaFunc net_funcs[] = {
    {"listen", net_listen, 1}, {"accept", net_accept, 1}, {"connect", net_connect, 3},
    {"read", net_read, -1}, /* -1 = variable args */
    {"write", net_write, 2},   {"close", net_close, 1},   {NULL, NULL, 0}};

void vm_register_net_module(VM *vm) { vm_register_module(vm, "net", net_funcs, 6); }