/*
 * sha256.c — SHA-256 digest C module for TinyActor VM (lazy dylib
 * lib/sha256.dylib, OpenSSL-backed like lib/tls.c).
 *
 *   sha256.raw_new()                  -> a        handle (nil on failure)
 *   sha256.raw_update(h, s)           -> Int      1 ok / 0 error
 *   sha256.raw_update_at(h, s, off, len) -> Int    1 ok / 0 error
 *   sha256.raw_final(h)               -> a        hex digest (nil on error)
 *   sha256.raw_digest(s)              -> a        hex digest (nil on error)
 *
 * Handle: an opaque int — slot | generation << SLOT_BITS, the context
 * lives in the module's slot table (lib/buffer.c pattern). The handle is
 * a plain value, so copying it across an actor boundary copies an int,
 * never the context: one context, one owner, until raw_final takes it
 * out.
 *
 * The handle used to be a TA Bytes object holding the EVP_MD_CTX pointer.
 * TA deep-copies a value that crosses an actor boundary, and the copy
 * duplicated the BYTES — sender and receiver ended up with two handles
 * on ONE context, and the second EVP_MD_CTX_free was a double free
 * (ASan: "attempting double-free"). A handle that owns a resource cannot
 * be a copied value; a handle that names a resource can.
 *
 * Lifetime: raw_final consumes the context — it digests, frees and
 * releases the slot. A later call on that handle returns nil (or 0), so
 * a second final is inert instead of freeing twice. A copy of the handle
 * names the same slot: whoever finalises first wins, the other sees nil.
 * Recycled slots get a bumped generation, so a handle minted before a
 * final never aliases the context that later takes the same slot.
 *
 * The table is global, so it needs a lock — but the lock covers the
 * TABLE, not the hashing: EVP_DigestUpdate on a 16 KB block costs more
 * than the lookup, and holding one lock across it serialised every
 * worker (NWORKERS=4 used 1.2 of 8 cores instead of 3.7, and ran
 * slower than NWORKERS=1). A slot is pinned (busy) while its context
 * is in an EVP call: the pin keeps the slot from being finalised or
 * recycled underneath the call, and keeps a second thread from
 * touching one context (EVP_MD_CTX is not reentrant). Different
 * contexts hash in parallel; one context at a time.
 *
 * A pinned slot rejects other callers: an update on a busy handle
 * returns 0, a final on a busy handle returns nil. The only way to hit
 * that is two actors holding copies of ONE handle and using it at the
 * same time — a program bug (the digest is inherently sequential), and
 * a loud refusal beats a corrupted digest.
 *
 * Limits: MAX_SHA_SLOTS concurrent hashers; raw_new returns nil past
 * that. A digest abandoned by a dying actor leaks its context and slot
 * until VM exit (no finalizer, same trade as lib/tls.c's slots).
 *
 * update reads the string's bytes in place, so a big document is hashed
 * without copying it — that is what the offset/length pair buys.
 */

#include "ta.h"
#include <openssl/evp.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define SLOT_BITS 12
#define MAX_SHA_SLOTS (1 << SLOT_BITS)

typedef struct {
    EVP_MD_CTX *ctx; /* NULL = freed slot (on the free list) */
    unsigned int gen;
    int busy; /* an EVP call is in flight on ctx */
} ShaSlot;

static ShaSlot sha_slots[MAX_SHA_SLOTS];
static int sha_next_slot = 0;
static int sha_free_head = -1; /* linked list of freed slot indices */
static int sha_free_next[MAX_SHA_SLOTS];

/* The slot table is global (a handle is a plain int), so it needs a
 * lock: two workers allocating at the same time raced on the free list
 * and could be handed the SAME slot — one actor's final then freed the
 * other actor's context (heap corruption). The lock covers the TABLE
 * only: the lookup and the pin are microseconds, the EVP call is not
 * (see the header). Same table pattern as buffer.c / net.c / random.c. */
static pthread_mutex_t g_sha_lock = PTHREAD_MUTEX_INITIALIZER;

/* Decode a handle to its slot, or NULL if invalid: out of range, a
 * generation that no longer matches (the slot was recycled, so this stale
 * handle must not alias the new context), or a freed slot still awaiting
 * reuse (ctx == NULL — the context was already finalised).
 * Caller holds g_sha_lock. */
static ShaSlot *sha_get(int64_t h) {
    if (h < 0)
        return NULL;
    int slot = (int)(h & (MAX_SHA_SLOTS - 1));
    unsigned int gen = (unsigned int)(h >> SLOT_BITS);
    if (slot >= sha_next_slot || sha_slots[slot].gen != gen)
        return NULL;
    ShaSlot *s = &sha_slots[slot];
    if (!s->ctx)
        return NULL;
    return s;
}

/* Pin the slot named by h for one EVP call and return it, or NULL if
 * the handle is stale or the context is already in flight. The pin is
 * what makes it safe to call EVP outside the table lock: the slot can
 * neither be finalised (which would free the context) nor recycled
 * while the call runs, and no second thread can enter the same
 * context.
 * Caller must not hold g_sha_lock. */
static ShaSlot *sha_pin(int64_t h) {
    pthread_mutex_lock(&g_sha_lock);
    ShaSlot *s = sha_get(h);
    if (!s || s->busy) {
        pthread_mutex_unlock(&g_sha_lock);
        return NULL;
    }
    s->busy = 1;
    pthread_mutex_unlock(&g_sha_lock);
    return s;
}

/* Release a pin taken by sha_pin. */
static void sha_unpin(ShaSlot *s) {
    pthread_mutex_lock(&g_sha_lock);
    s->busy = 0;
    pthread_mutex_unlock(&g_sha_lock);
}

/* Allocate a handle from the free list or the next fresh slot.
 * Recycled slots get a bumped generation so every handle minted before
 * the final decodes to NULL (stale) instead of aliasing the new context.
 * Caller holds g_sha_lock. */
static int64_t sha_alloc_handle(void) {
    int slot;
    if (sha_free_head >= 0) {
        slot = sha_free_head;
        sha_free_head = sha_free_next[slot];
        sha_free_next[slot] = -1;
        sha_slots[slot].gen++;
    } else {
        if (sha_next_slot >= MAX_SHA_SLOTS)
            return -1;
        slot = sha_next_slot++;
        sha_slots[slot].gen = 0;
    }
    return (int64_t)slot + ((int64_t)sha_slots[slot].gen << SLOT_BITS);
}

/* Release a slot back to the free list. The context leaves the table
 * here, but the caller still owns it and frees it — the digest runs
 * outside the lock. Caller holds g_sha_lock. */
static void sha_release_slot(int slot) {
    sha_slots[slot].ctx = NULL;
    sha_free_next[slot] = sha_free_head;
    sha_free_head = slot;
}

/* hex: the digest as 64 lowercase hex chars (SHA-256 is 32 bytes). */
static Val sha_hex(Proc *p, const unsigned char *md, unsigned int mdlen) {
    static const char digits[] = "0123456789abcdef";
    char hex[65];
    if (mdlen != 32)
        return val_nil();
    for (unsigned int i = 0; i < mdlen; i++) {
        hex[2 * i] = digits[md[i] >> 4];
        hex[2 * i + 1] = digits[md[i] & 0xf];
    }
    hex[64] = '\0';
    return val_string(p, hex, 64);
}

static Val sha_new(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)args;
    (void)nargs;
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    if (!c)
        return val_nil();
    if (EVP_DigestInit_ex(c, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(c);
        return val_nil();
    }
    pthread_mutex_lock(&g_sha_lock);
    int64_t h = sha_alloc_handle();
    if (h < 0) {
        pthread_mutex_unlock(&g_sha_lock);
        EVP_MD_CTX_free(c);
        return val_nil();
    }
    int slot = (int)(h & (MAX_SHA_SLOTS - 1)); /* h is slot | gen<<SLOT_BITS */
    sha_slots[slot].ctx = c;
    pthread_mutex_unlock(&g_sha_lock);
    return val_int(h);
}

/* update_at: hash s[off, off+len). The whole string is off=0, len=length. */
static Val sha_update_at(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_int(args[0]) || !val_is_string(args[1]) || !val_is_int(args[2]) ||
        !val_is_int(args[3]))
        return val_int(0);
    int64_t off = val_get_int(args[2]);
    int64_t len = val_get_int(args[3]);
    HeapString *hs = val_get_string(args[1]);
    if (off < 0 || len < 0 || off + len > hs->len)
        return val_int(0);
    ShaSlot *s = sha_pin(val_get_int(args[0]));
    if (!s)
        return val_int(0);
    int ok = len == 0 || EVP_DigestUpdate(s->ctx, hs->data + off, (size_t)len) == 1;
    sha_unpin(s);
    return val_int(ok);
}

static Val sha_update(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_int(args[0]) || !val_is_string(args[1]))
        return val_int(0);
    HeapString *hs = val_get_string(args[1]);
    if (hs->len == 0) {
        /* An empty string is still a valid digest input — but the handle
         * must be live, so the slot check still runs. */
        ShaSlot *s = sha_pin(val_get_int(args[0]));
        if (!s)
            return val_int(0);
        sha_unpin(s);
        return val_int(1);
    }
    ShaSlot *s = sha_pin(val_get_int(args[0]));
    if (!s)
        return val_int(0);
    int ok = EVP_DigestUpdate(s->ctx, hs->data, (size_t)hs->len) == 1;
    sha_unpin(s);
    return val_int(ok);
}

/* final: consume the context. Takes it out of the slot under the lock
 * (so a second final of the same handle, or of a copy of it, finds an
 * empty slot and returns nil instead of freeing the context twice),
 * then digests and frees it outside the lock — the context is ours
 * alone by then. */
static Val sha_final(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_int(args[0]))
        return val_nil();
    pthread_mutex_lock(&g_sha_lock);
    ShaSlot *s = sha_get(val_get_int(args[0]));
    if (!s || s->busy) {
        pthread_mutex_unlock(&g_sha_lock);
        return val_nil();
    }
    EVP_MD_CTX *c = s->ctx;
    int slot = (int)(val_get_int(args[0]) & (MAX_SHA_SLOTS - 1));
    sha_release_slot(slot);
    pthread_mutex_unlock(&g_sha_lock);

    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdlen = 0;
    int ok = EVP_DigestFinal_ex(c, md, &mdlen) == 1;
    EVP_MD_CTX_free(c);
    if (!ok)
        return val_nil();
    return sha_hex(tls_current_proc, md, mdlen);
}

static Val sha_digest(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!val_is_string(args[0]))
        return val_nil();
    HeapString *hs = val_get_string(args[0]);
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdlen = 0;
    if (EVP_Digest(hs->data, (size_t)hs->len, md, &mdlen, EVP_sha256(), NULL) != 1)
        return val_nil();
    return sha_hex(tls_current_proc, md, mdlen);
}

static TaFunc sha_funcs[] = {
    {"raw_new", sha_new, 0},     {"raw_update", sha_update, 2}, {"raw_update_at", sha_update_at, 4},
    {"raw_final", sha_final, 1}, {"raw_digest", sha_digest, 1}, {NULL, NULL, 0}};

/* Lazy module entry: the loader dlsym's "vm_load_self" after dlopen
 * (main.c find_cfunc_autoload). os.c/tls.c registration pattern: dotted
 * names, NO module-registry entry — a registry entry for "sha256" would
 * make `import sha256` a compile-time no-op and lib/sha256.ta (the facade)
 * would never load. */
void vm_load_self(VM *vm) {
    for (int i = 0; sha_funcs[i].name != NULL; i++) {
        char qualified[64];
        snprintf(qualified, sizeof(qualified), "sha256.%s", sha_funcs[i].name);
        vm_register(vm, qualified, sha_funcs[i].fn, sha_funcs[i].nargs);
    }
}