/*
 * tls.c — TLS client C module for TinyActor VM (lazy dylib lib/tls.dylib,
 * stdlib-port-plan Phase 6 #1)
 *
 * OpenSSL-backed TLS streams following src/net.c's signal protocol
 * (docs/c-module.md §4): the C side returns atomic signals, lib/tls.ta
 * lifts them into typed values.
 *
 *   tls.raw_wrap(fd, host) -> Resource | -1   build the SSL*, bind the fd,
 *                                             send SNI — never yields;
 *   tls.raw_hs(h)          -> 1 | -1          one SSL_connect step,
 *                                             resumes across suspensions;
 *   tls.raw_read(h, n)     -> string | 'eof | nil | -1 | -2
 *   tls.raw_write(h, s)    -> int | nil | -1 | -2
 *   tls.raw_close(h, keep) -> 1 | fd(keep) | -1 | -2
 *
 * The handle is an opaque Resource (docs/c-resource-design.md): the
 * actor-heap header (one per holder, GC-moved) carries owner_pid, the
 * shared ResourcePayload carries released + TlsState{SSL*, fd} outside
 * every actor heap. The slot table, the generation handles, the
 * pid-keyed in-flight handshake list and g_tls_lock are all gone — no
 * table, no module mutex, nothing that could race a GC or another
 * actor's close.
 *
 * Signals and errno (read back via net.errno()):
 *   -1 invalid / closed — EBADF (released payload or forged value;
 *        same observation as v1's stale handle);
 *   -2 not owner — EPERM (the handle was sent to another actor; this
 *        copy must never touch the receiver's SSL*);
 *   otherwise unchanged from v1: SSL_ERROR_SYSCALL keeps the syscall
 *   errno, a protocol failure (SSL_ERROR_SSL) reports EPROTO, a
 *   mid-handshake EOF reports ECONNRESET.
 * nil means the actor suspended on WANT_READ / WANT_WRITE
 * (vm_watch_fd + vm_yield, the net.c idiom). Under the three-phase
 * ccall protocol (src/vm.c do_ccall) the nil return is DISCARDED and
 * the whole call instruction re-executes once the fd is ready — the
 * TA-side nil arms match the net/tcp shape and never actually fire.
 *
 * Why the handshake is split across raw_wrap / raw_hs:
 *   - a ccall that yields must not hold a fresh resource — do_ccall
 *     fatals on `arena.resources` at yield, dropping the CallbackArena
 *     result would strand the payload;
 *   - an in-flight SSL* must survive re-entry, and the old answer (a
 *     module list keyed by proc pid) was exactly the global state this
 *     migration exists to delete.
 * So raw_wrap builds and returns the Resource in one non-yielding call,
 * and raw_hs drives SSL_connect with the handle already in its own
 * arguments — the re-executed instruction finds the SSL* through the
 * value, no pid keying, no stash. (Several in-flight handshakes per
 * actor are now representable; the TA facade drives them one at a time.)
 *
 * A v1 handshake has NO deadline: a peer that accepts and then never
 * speaks suspends the actor forever (documented hang surface, same
 * trade as net_accept having no timeout).
 *
 * Release paths (docs/c-resource-design.md §6): raw_close releases the
 * payload explicitly; an actor dying, a GC collecting the last header,
 * or the VM tearing down with the handle still live runs tls_dtor as
 * the fallback — best-effort SSL_shutdown (close_notify on a
 * non-blocking fd cannot block), SSL_free, close(fd), free(state); no
 * TA allocation, no VM entry, legal in all four contexts. On a terminal
 * handshake failure the fd is handed back first (s->fd = -1): the
 * caller still owns the TcpConn it unwrapped, exactly v1's contract —
 * and close_keep_fd hands the fd back the same way before releasing.
 *
 * Loading follows the lazy-dylib convention (lib/random.c pattern): this
 * file lives in lib/tls.c, the loader dlopen's lib/tls.dylib on the first
 * tls.* reference and enters through vm_load_self. Registration itself
 * follows the lib/os.c pattern, NOT a module-registry entry: the
 * primitives are registered under their dotted names ("tls.raw_*")
 * directly. A registry entry for "tls" would make `import tls` a
 * compile-time no-op (is_builtin_module) and lib/tls.ta — the facade that
 * owns the public API — would never load.
 *
 * Certificate verification: OFF (SSL_VERIFY_NONE, OpenSSL default for
 * TLS_client_method). v1 ships transport encryption only — no CA
 * validation, no hostname matching. This is a documented limitation,
 * not an oversight; turning it on drags in a trust-store story the
 * stdlib plan has not settled.
 *
 * The SSL_CTX is built once (pthread_once) and read-only afterwards: a
 * module constant, not per-resource state.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "ta.h"
#include <errno.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    SSL *ssl;
    int fd; /* -1 once the fd has been handed back to the caller */
} TlsState;

static SSL_CTX *g_tls_ctx = NULL;
static pthread_once_t g_tls_ctx_once = PTHREAD_ONCE_INIT;

static void tls_ctx_create(void) {
    OPENSSL_init_ssl(0, NULL);
    g_tls_ctx = SSL_CTX_new(TLS_client_method());
    /* Verification stays off in v1 — see the header note. */
}

static SSL_CTX *tls_ctx(void) {
    pthread_once(&g_tls_ctx_once, tls_ctx_create);
    return g_tls_ctx;
}

/* The fallback cleanup (GC / proc_die / vm_free) and the explicit close
 * share this one body: release() runs it exactly once, after which
 * `released` on the payload blocks every copy. SSL_shutdown is
 * best-effort — non-blocking fd, one attempt, queue drained — and the
 * fd is skipped when it was handed back (handshake failure /
 * close_keep_fd). */
static void tls_dtor(void *p) {
    TlsState *st = (TlsState *)p;
    if (st->ssl) {
        SSL_shutdown(st->ssl);
        ERR_clear_error();
        SSL_free(st->ssl);
    }
    if (st->fd >= 0)
        close(st->fd);
    free(st);
}

/* tls_open — resolve + validate a handle for one operation.
 * Returns 0 with the payload handed out, or the signal to relay: -1
 * invalid / released, -2 not owner. The header is read only here; the
 * caller works through the payload, which nothing ever moves. */
static int tls_open(Val v, ResourcePayload **out) {
    HeapResource *r = val_get_resource(v);
    if (r == NULL || r->res->released)
        return -1;
    if (r->owner_pid != tls_current_proc->pid)
        return -2;
    *out = r->res;
    return 0;
}

/* errno for a tls_open failure — net.errno() reads it back in TA.
 * EBADF matches v1's stale-handle report; EPERM is the new not-owner
 * signal (the copy was sent away). */
static void tls_open_errno(int code) {
    tls_current_proc->last_errno = (code == -2) ? EPERM : EBADF;
}

/* Store the errno for a failed TLS operation on the calling proc (the
 * net.c last_errno convention, read back via net.errno()):
 *   SYSCALL with errno   -> that errno
 *   SYSCALL with errno 0 -> ECONNRESET (unexpected EOF at the syscall
 *                           layer: the peer hung up mid-operation)
 *   SSL (protocol)       -> EPROTO
 *   anything else        -> ECONNRESET
 * errno is trusted ONLY on SSL_ERROR_SYSCALL: SSL_read/SSL_write do not
 * guarantee setting it on their own failures, so a syscall from an
 * earlier VM step (poll et al.) would otherwise leak a stale errno —
 * possibly EAGAIN — into the caller's error report. The OpenSSL error
 * queue is drained either way so a later failure is never reported
 * through a stale queue entry. */
static void tls_record_error(int err) {
    if (err == SSL_ERROR_SSL) {
#ifdef EPROTO
        tls_current_proc->last_errno = EPROTO;
#else
        tls_current_proc->last_errno = EIO;
#endif
    } else if (err == SSL_ERROR_SYSCALL && errno != 0) {
        tls_current_proc->last_errno = errno;
    } else {
        tls_current_proc->last_errno = ECONNRESET;
    }
    ERR_clear_error();
}

static Val tls_sym_eof(VM *vm) {
    int idx = vm_intern_symbol(vm, "eof");
    return val_symbol((uint32_t)idx);
}

/* raw_wrap(fd, host) -> Resource | -1. Wrap an existing non-blocking
 * socket fd (from a TcpConn unwrap) into a TLS client: SSL_new,
 * SSL_set_fd, SNI — then hand back the Resource. Handshake progress is
 * raw_hs's job (see the header for why the split exists). The fd
 * belongs to the TlsConn only once the handshake succeeds; on any
 * failure path here nothing is released (no Resource exists yet) and
 * the fd stays with the caller. */
static Val tls_raw_wrap(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_int(args[0])) {
        tls_current_proc->last_errno = EINVAL;
        return val_int(-1);
    }
    int fd = (int)val_get_int(args[0]);
    if (fd < 0) {
        tls_current_proc->last_errno = EBADF;
        return val_int(-1);
    }
    SSL_CTX *ctx = tls_ctx();
    if (!ctx) {
        tls_current_proc->last_errno = ENOMEM;
        return val_int(-1);
    }
    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        tls_current_proc->last_errno = ENOMEM;
        return val_int(-1);
    }
    if (SSL_set_fd(ssl, fd) != 1) {
        tls_current_proc->last_errno = EINVAL;
        ERR_clear_error();
        SSL_free(ssl);
        return val_int(-1);
    }
    if (val_is_string(args[1]) && val_get_string(args[1])->len > 0) {
        HeapString *host = val_get_string(args[1]);
        char *name = malloc((size_t)host->len + 1);
        if (name) { /* SNI is best-effort: OOM just skips it */
            memcpy(name, host->data, host->len);
            name[host->len] = '\0';
            SSL_set_tlsext_host_name(ssl, name);
            free(name);
        }
    }
    TlsState *st = (TlsState *)malloc(sizeof(TlsState));
    if (!st) {
        tls_current_proc->last_errno = ENOMEM;
        ERR_clear_error();
        SSL_free(ssl);
        return val_int(-1);
    }
    st->ssl = ssl;
    st->fd = fd;
    /* Never yields from here on — a fresh resource inside a yielding
     * ccall would be dropped with the arena (do_ccall fatal). */
    return ta_resource_new(tls_current_proc, st, tls_dtor);
}

/* raw_hs(h) -> 1 | -1 | nil(suspended, never delivered — see header).
 * One SSL_connect step; WANT_READ / WANT_WRITE parks the actor on the
 * fd and the re-executed call finds the SSL* through its own argument.
 * Terminal failure records the errno, hands the fd back to the caller
 * (s->fd = -1, v1 contract: only the SSL* goes) and releases the
 * resource, so GC / death can no longer double-free or close the
 * caller's fd. */
static Val tls_raw_hs(VM *vm, Val *args, int nargs) {
    (void)nargs;
    ResourcePayload *pl;
    int code = tls_open(args[0], &pl);
    if (code < 0) {
        tls_open_errno(code);
        return val_int(code);
    }
    TlsState *st = (TlsState *)pl->ptr;
    int ret = SSL_connect(st->ssl);
    if (ret == 1)
        return val_int(1);

    int err = SSL_get_error(st->ssl, ret);
    if (err == SSL_ERROR_WANT_READ) {
        vm_watch_fd(vm, st->fd, POLLIN);
        vm_yield(vm);
        return val_nil();
    }
    if (err == SSL_ERROR_WANT_WRITE) {
        vm_watch_fd(vm, st->fd, POLLOUT);
        vm_yield(vm);
        return val_nil();
    }

    tls_record_error(err);
    st->fd = -1; /* fd back to the caller before the one-shot release */
    ta_resource_release(val_get_resource(args[0]));
    return val_int(-1);
}

/* raw_read(h, n) -> string | 'eof | nil | -1 | -2. Same shape and clamping
 * as net_read (n defaults to 4096, clamped to 1..65536) so tls.read is
 * a drop-in bufio read_fn. */
static Val tls_raw_read(VM *vm, Val *args, int nargs) {
    (void)nargs;
    ResourcePayload *pl;
    int code = tls_open(args[0], &pl);
    if (code < 0) {
        tls_open_errno(code);
        return val_int(code);
    }
    TlsState *st = (TlsState *)pl->ptr;
    int max_len = 4096;
    if (val_is_int(args[1]))
        max_len = (int)val_get_int(args[1]);
    if (max_len <= 0)
        max_len = 4096;
    if (max_len > 65536)
        max_len = 65536;

    char *buf = malloc((size_t)max_len);
    if (!buf) {
        tls_current_proc->last_errno = ENOMEM;
        return val_int(-1);
    }
    int n = SSL_read(st->ssl, buf, max_len);
    if (n > 0) {
        Val result = val_string(tls_current_proc, buf, n);
        free(buf);
        return result;
    }
    free(buf);

    int err = SSL_get_error(st->ssl, n);
    if (err == SSL_ERROR_ZERO_RETURN)
        return tls_sym_eof(vm); /* clean close_notify from the peer */
    if (err == SSL_ERROR_WANT_READ) {
        vm_watch_fd(vm, st->fd, POLLIN);
        vm_yield(vm);
        return val_nil();
    }
    if (err == SSL_ERROR_WANT_WRITE) {
        vm_watch_fd(vm, st->fd, POLLOUT);
        vm_yield(vm);
        return val_nil();
    }
    tls_record_error(err);
    return val_int(-1);
}

/* raw_write(h, str) -> bytes | nil | -1 | -2. One SSL_write step;
 * partial writes return the count and the TA layer loops (tcp.write_all
 * shape). */
static Val tls_raw_write(VM *vm, Val *args, int nargs) {
    (void)nargs;
    ResourcePayload *pl;
    int code = tls_open(args[0], &pl);
    if (code < 0) {
        tls_open_errno(code);
        return val_int(code);
    }
    TlsState *st = (TlsState *)pl->ptr;
    if (!val_is_string(args[1])) {
        tls_current_proc->last_errno = EINVAL;
        return val_int(-1);
    }
    HeapString *hs = val_get_string(args[1]);

    int n = SSL_write(st->ssl, hs->data, (int)hs->len);
    if (n > 0)
        return val_int(n);

    int err = SSL_get_error(st->ssl, n);
    if (err == SSL_ERROR_WANT_READ) {
        vm_watch_fd(vm, st->fd, POLLIN);
        vm_yield(vm);
        return val_nil();
    }
    if (err == SSL_ERROR_WANT_WRITE) {
        vm_watch_fd(vm, st->fd, POLLOUT);
        vm_yield(vm);
        return val_nil();
    }
    tls_record_error(err);
    return val_int(-1);
}

/* raw_close(h, keep_fd): release the resource — which runs tls_dtor
 * (best-effort SSL_shutdown + SSL_free + close(fd) + free(state)).
 * keep_fd != 0 hands the fd back first (s->fd = -1) so the dtor leaves
 * it open, and the fd is returned for the caller to keep as a plain
 * TcpConn. Idempotent across every stale copy: `released` lives on the
 * shared payload, so a repeat close returns 1 without touching the
 * other holder; a copy owned by another actor returns -2 and can never
 * close the receiver's connection. */
static Val tls_raw_close(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    HeapResource *r = val_get_resource(args[0]);
    if (r == NULL)
        return val_int(-1);
    if (r->owner_pid != tls_current_proc->pid) {
        tls_current_proc->last_errno = EPERM;
        return val_int(-2);
    }
    if (r->res->released)
        return val_int(1);
    TlsState *st = (TlsState *)r->res->ptr;
    int keep_fd = val_is_int(args[1]) && val_get_int(args[1]) != 0;
    int fd = st->fd;
    if (keep_fd)
        st->fd = -1;
    ta_resource_release(r);
    return val_int(keep_fd ? fd : 1);
}

static TaFunc tls_funcs[] = {{"raw_wrap", tls_raw_wrap, 2},   {"raw_hs", tls_raw_hs, 1},
                             {"raw_read", tls_raw_read, 2},   {"raw_write", tls_raw_write, 2},
                             {"raw_close", tls_raw_close, 2}, {NULL, NULL, 0}};

/* Lazy module entry: the loader dlsym's "vm_load_self" after dlopen
 * (main.c find_cfunc_autoload). os.c registration pattern: dotted names,
 * NO module-registry entry — see the header comment for why ("import tls"
 * must load lib/tls.ta). */
void vm_load_self(VM *vm) {
    for (int i = 0; tls_funcs[i].name != NULL; i++) {
        char qualified[64];
        snprintf(qualified, sizeof(qualified), "tls.%s", tls_funcs[i].name);
        vm_register(vm, qualified, tls_funcs[i].fn, tls_funcs[i].nargs);
    }
}