/*
 * tls.c — TLS client C module for TinyActor VM (static, stdlib-port-plan
 * Phase 6 #1)
 *
 * OpenSSL-backed TLS streams following src/net.c's signal protocol
 * (docs/c-module.md §4): the C side returns atomic signals, lib/tls.ta
 * lifts them into typed values.
 *
 *   tls.raw_wrap(fd, host)  -> handle (int) | nil (suspended, retry) | -1
 *   tls.raw_read(h, n)      -> string | 'eof | nil (suspended, retry) | -1
 *   tls.raw_write(h, s)     -> bytes written (int) | nil (suspended) | -1
 *   tls.raw_close(h, keep)  -> keep!=0: the underlying fd (TLS-only
 *                              shutdown, fd stays open — the caller can
 *                              keep it as a plain TcpConn); keep==0: 1
 *                              (TLS shutdown + fd closed). Stale handle
 *                              -> -1; double close of the SAME handle
 *                              value is idempotent (1).
 *
 * -1 carries the errno on the calling proc's last_errno (read back via
 * net.errno()): SSL_ERROR_SYSCALL keeps the syscall errno, a protocol
 * failure (SSL_ERROR_SSL) reports EPROTO, a mid-handshake EOF reports
 * ECONNRESET. nil means the actor was suspended on WANT_READ /
 * WANT_WRITE (vm_watch_fd + vm_yield, the net.c idiom) and the TA layer
 * must retry the same call.
 *
 * Registration follows the src/os.c pattern, NOT a module-registry
 * entry: the primitives are registered under their dotted names
 * ("tls.raw_*") directly. A registry entry for "tls" would make
 * `import tls` a compile-time no-op (is_builtin_module) and lib/tls.ta —
 * the facade that owns the public API — would never load.
 *
 * Handshake progress (raw_wrap): SSL_connect on a non-blocking fd
 * returns WANT_READ / WANT_WRITE; the in-flight SSL* lives in the slot
 * table and is keyed for re-entry by proc pid — an actor runs
 * sequentially, so at most one handshake is in flight per pid (same
 * assumption as net.c's connects table). The caller must retry
 * raw_wrap with the same arguments; the fd argument is ignored on
 * re-entry. A v1 handshake has NO deadline: a peer that accepts and
 * then never speaks suspends the actor forever (documented leak/hang
 * surface, same trade as net_accept having no timeout).
 *
 * Registry: slot | generation table (lib/process.c pattern), so a
 * closed handle's recycled slot can never alias an old handle value.
 * Thread safety: none — plain globals, same data-race surface as
 * lib/buffer.c / lib/process.c today (concurrent actors are v1-unsafe).
 *
 * Certificate verification: OFF (SSL_VERIFY_NONE, OpenSSL default for
 * TLS_client_method). v1 ships transport encryption only — no CA
 * validation, no hostname matching. This is a documented limitation,
 * not an oversight; turning it on drags in a trust-store story the
 * stdlib plan has not settled.
 *
 * Leak surface (docs/c-module.md §5): an actor that dies mid-handshake
 * leaks its slot + SSL* + fd until VM exit; the registry itself is
 * process-lifetime (tavm runs one VM per process, like net.c).
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

#define TLS_SLOT_BITS 8
#define MAX_TLS_SLOTS (1 << TLS_SLOT_BITS)

typedef struct {
    SSL *ssl; /* NULL = freed slot (on the free list) */
    int fd;
    int gen;
} TlsSlot;

static TlsSlot tls_slots[MAX_TLS_SLOTS];
static int tls_next_slot = 0;
static int tls_free_head = -1; /* linked list of freed slot indices */
static int tls_free_next[MAX_TLS_SLOTS];

/* In-flight handshakes, keyed by proc pid (see header). */
typedef struct TlsHandshake {
    int pid;
    int slot;
    struct TlsHandshake *next;
} TlsHandshake;

static TlsHandshake *tls_handshakes = NULL;

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

/* Decode a handle to its slot, or NULL if invalid: out of range, a
 * generation that no longer matches (slot closed and recycled), or a
 * freed slot still awaiting reuse (ssl == NULL — covers the stale
 * operation case; raw_close treats the same handle value as an
 * idempotent double close before anyone else looks here). */
static TlsSlot *tls_get(int64_t h) {
    if (h < 0)
        return NULL;
    int slot = (int)(h & (MAX_TLS_SLOTS - 1));
    int gen = (int)(h >> TLS_SLOT_BITS);
    if (slot >= tls_next_slot || tls_slots[slot].gen != gen)
        return NULL;
    return &tls_slots[slot];
}

/* Allocate a handle from the free list or the next fresh slot; recycled
 * slots get a bumped generation so every handle minted before the close
 * decodes to NULL (stale) instead of aliasing the new connection. */
static int64_t tls_alloc_handle(void) {
    int slot;
    if (tls_free_head >= 0) {
        slot = tls_free_head;
        tls_free_head = tls_free_next[slot];
        tls_free_next[slot] = -1;
        tls_slots[slot].gen++;
    } else {
        if (tls_next_slot >= MAX_TLS_SLOTS)
            return -1;
        slot = tls_next_slot++;
        tls_slots[slot].gen = 0;
    }
    return (int64_t)slot + ((int64_t)tls_slots[slot].gen << TLS_SLOT_BITS);
}

static void tls_release_slot(int slot) {
    tls_slots[slot].ssl = NULL;
    tls_slots[slot].fd = -1;
    tls_free_next[slot] = tls_free_head;
    tls_free_head = slot;
}

static TlsHandshake *tls_hs_find(int pid) {
    for (TlsHandshake *hs = tls_handshakes; hs; hs = hs->next)
        if (hs->pid == pid)
            return hs;
    return NULL;
}

static void tls_hs_add(int pid, int slot) {
    TlsHandshake *hs = malloc(sizeof(*hs));
    if (!hs)
        return; /* the slot is orphaned — same OOM posture as net.c */
    hs->pid = pid;
    hs->slot = slot;
    hs->next = tls_handshakes;
    tls_handshakes = hs;
}

static void tls_hs_remove(int pid) {
    for (TlsHandshake **p = &tls_handshakes; *p; p = &(*p)->next) {
        if ((*p)->pid == pid) {
            TlsHandshake *dead = *p;
            *p = dead->next;
            free(dead);
            return;
        }
    }
}

/* Store the errno for a failed TLS operation on the calling proc (the
 * net.c last_errno convention, read back via net.errno()):
 *   SYSCALL with errno   -> that errno
 *   SYSCALL with errno 0 -> ECONNRESET (unexpected EOF at the syscall
 *                           layer: the peer hung up mid-operation)
 *   SSL (protocol)       -> EPROTO
 * The OpenSSL error queue is drained either way so a later failure is
 * never reported through a stale queue entry. */
static void tls_record_error(int err) {
    if (err == SSL_ERROR_SSL) {
#ifdef EPROTO
        tls_current_proc->last_errno = EPROTO;
#else
        tls_current_proc->last_errno = EIO;
#endif
    } else if (errno != 0) {
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

/* raw_wrap(fd, host) -> handle | nil | -1. Wraps an existing
 * non-blocking socket fd (from a TcpConn unwrap) into a TLS client and
 * runs the handshake to completion across suspensions. See the header
 * for the retry contract; `host` is the SNI server name (empty/nil =
 * none sent). */
static Val tls_raw_wrap(VM *vm, Val *args, int nargs) {
    (void)nargs;
    if (!val_is_int(args[0]))
        return val_int(-1);
    int fd = (int)val_get_int(args[0]);
    if (fd < 0)
        return val_int(-1);
    SSL_CTX *ctx = tls_ctx();
    if (!ctx)
        return val_int(-1);

    int slot;
    if (tls_hs_find(tls_current_proc->pid)) {
        /* Re-entry after a suspension: resume the handshake in flight
         * for this pid; the fd/host arguments are ignored. */
        slot = tls_hs_find(tls_current_proc->pid)->slot;
    } else {
        SSL *ssl = SSL_new(ctx);
        if (!ssl)
            return val_int(-1);
        if (SSL_set_fd(ssl, fd) != 1) {
            ERR_clear_error();
            SSL_free(ssl);
            return val_int(-1);
        }
        if (val_is_string(args[1]) && val_get_string(args[1])->len > 0) {
            HeapString *host = val_get_string(args[1]);
            char *name = malloc((size_t)host->len + 1);
            if (name) { /* SNI is best-effort: OOM just skips it */
                memcpy(name, host->data, (size_t)host->len);
                name[host->len] = '\0';
                SSL_set_tlsext_host_name(ssl, name);
                free(name);
            }
        }
        int64_t handle = tls_alloc_handle();
        if (handle < 0) {
            ERR_clear_error();
            SSL_free(ssl);
            return val_int(-1);
        }
        slot = (int)(handle & (MAX_TLS_SLOTS - 1));
        tls_slots[slot].ssl = ssl;
        tls_slots[slot].fd = fd;
        tls_hs_add(tls_current_proc->pid, slot);
    }

    SSL *ssl = tls_slots[slot].ssl;
    int ret = SSL_connect(ssl);
    if (ret == 1) {
        tls_hs_remove(tls_current_proc->pid);
        return val_int((int64_t)slot + ((int64_t)tls_slots[slot].gen << TLS_SLOT_BITS));
    }

    int err = SSL_get_error(ssl, ret);
    if (err == SSL_ERROR_WANT_READ) {
        vm_watch_fd(vm, tls_slots[slot].fd, POLLIN);
        vm_yield(vm);
        return val_nil();
    }
    if (err == SSL_ERROR_WANT_WRITE) {
        vm_watch_fd(vm, tls_slots[slot].fd, POLLOUT);
        vm_yield(vm);
        return val_nil();
    }

    /* Terminal handshake failure: the fd stays open (the caller still
     * owns the TcpConn it unwrapped); only the SSL* goes. */
    tls_record_error(err);
    tls_hs_remove(tls_current_proc->pid);
    SSL_free(ssl);
    tls_release_slot(slot);
    return val_int(-1);
}

/* raw_read(h, n) -> string | 'eof | nil | -1. Same shape and clamping
 * as net_read (n defaults to 4096, clamped to 1..65536) so tls.read is
 * a drop-in bufio read_fn. */
static Val tls_raw_read(VM *vm, Val *args, int nargs) {
    (void)nargs;
    TlsSlot *s = tls_get(val_get_int(args[0]));
    if (!s || !s->ssl) {
        tls_current_proc->last_errno = EBADF;
        return val_int(-1);
    }
    int max_len = 4096;
    if (val_is_int(args[1]))
        max_len = (int)val_get_int(args[1]);
    if (max_len <= 0)
        max_len = 4096;
    if (max_len > 65536)
        max_len = 65536;

    char *buf = malloc((size_t)max_len);
    if (!buf)
        return val_int(-1);
    int n = SSL_read(s->ssl, buf, max_len);
    if (n > 0) {
        Val result = val_string(tls_current_proc, buf, n);
        free(buf);
        return result;
    }
    free(buf);

    int err = SSL_get_error(s->ssl, n);
    if (err == SSL_ERROR_ZERO_RETURN)
        return tls_sym_eof(vm); /* clean close_notify from the peer */
    if (err == SSL_ERROR_WANT_READ) {
        vm_watch_fd(vm, s->fd, POLLIN);
        vm_yield(vm);
        return val_nil();
    }
    if (err == SSL_ERROR_WANT_WRITE) {
        vm_watch_fd(vm, s->fd, POLLOUT);
        vm_yield(vm);
        return val_nil();
    }
    tls_record_error(err);
    return val_int(-1);
}

/* raw_write(h, str) -> bytes | nil | -1. One SSL_write step; partial
 * writes return the count and the TA layer loops (tcp.write_all
 * shape). */
static Val tls_raw_write(VM *vm, Val *args, int nargs) {
    (void)nargs;
    TlsSlot *s = tls_get(val_get_int(args[0]));
    if (!s || !s->ssl) {
        tls_current_proc->last_errno = EBADF;
        return val_int(-1);
    }
    if (!val_is_string(args[1]))
        return val_int(-1);
    HeapString *hs = val_get_string(args[1]);

    int n = SSL_write(s->ssl, hs->data, (int)hs->len);
    if (n > 0)
        return val_int(n);

    int err = SSL_get_error(s->ssl, n);
    if (err == SSL_ERROR_WANT_READ) {
        vm_watch_fd(vm, s->fd, POLLIN);
        vm_yield(vm);
        return val_nil();
    }
    if (err == SSL_ERROR_WANT_WRITE) {
        vm_watch_fd(vm, s->fd, POLLOUT);
        vm_yield(vm);
        return val_nil();
    }
    tls_record_error(err);
    return val_int(-1);
}

/* raw_close(h, keep_fd): one best-effort SSL_shutdown (close_notify;
 * the bi-directional wait would need its own suspension machinery for
 * zero user value — the fd is going away right after), then close the
 * fd unless keep_fd != 0. keep_fd != 0 returns the underlying fd so
 * the caller can keep it as a plain TcpConn. Idempotent for the SAME
 * handle value (1); a stale handle returns -1. */
static Val tls_raw_close(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    int64_t h = val_get_int(args[0]);
    if (h < 0)
        return val_int(-1);
    int slot = (int)(h & (MAX_TLS_SLOTS - 1));
    int gen = (int)(h >> TLS_SLOT_BITS);
    if (slot >= tls_next_slot || tls_slots[slot].gen != gen)
        return val_int(-1);
    if (!tls_slots[slot].ssl)
        return val_int(1); /* double close of the same handle */

    SSL_shutdown(tls_slots[slot].ssl);
    ERR_clear_error();
    SSL_free(tls_slots[slot].ssl);
    int keep_fd = val_is_int(args[1]) && val_get_int(args[1]) != 0;
    int fd = tls_slots[slot].fd;
    tls_release_slot(slot);
    if (keep_fd)
        return val_int(fd);
    close(fd);
    return val_int(1);
}

static TaFunc tls_funcs[] = {{"raw_wrap", tls_raw_wrap, 2},
                             {"raw_read", tls_raw_read, 2},
                             {"raw_write", tls_raw_write, 2},
                             {"raw_close", tls_raw_close, 2},
                             {NULL, NULL, 0}};

/* os.c registration pattern: dotted names, NO module-registry entry —
 * see the header comment for why ("import tls" must load lib/tls.ta). */
void vm_register_tls_module(VM *vm) {
    for (int i = 0; tls_funcs[i].name != NULL; i++) {
        char qualified[64];
        snprintf(qualified, sizeof(qualified), "tls.%s", tls_funcs[i].name);
        vm_register(vm, qualified, tls_funcs[i].fn, tls_funcs[i].nargs);
    }
}