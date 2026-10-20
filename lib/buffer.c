/*
 * lib/buffer.c — buffer module for TinyActor (built as lib/buffer.dylib / .so)
 *
 * Mutable byte buffer: the first practitioner of the C mutable-resource
 * handle convention (docs/c-module.md §5). Storage layout is the global
 * table + int handle pattern that src/buf.c established (A4 removed that
 * module; this is the only remaining table implementation).
 *
 * The C side works on BARE int handles; the TA side (lib/buffer.ta)
 * wraps them in the single-constructor ADT `type Buffer { Buf(int) }`.
 * Forgery is prevented by the typechecker (the constructor is the
 * capability), so the C side never validates tags — it only validates
 * handle liveness.
 *
 *   raw_new()                -> Int handle (>= 0), or -1 on table
 *                               full/OOM (TA lifts to
 *                               Err("buffer table full"))
 *   raw_push_str(h, s)       -> Int new length, or -1 (closed / stale /
 *                               non-string)
 *   raw_push_byte(h, n)      -> Int new length, or -1 (closed / stale /
 *                               n outside 0..255 — defense in depth;
 *                               lib/buffer.ta pre-validates the range)
 *   raw_to_string(h)         -> String snapshot (buffer is retained), or
 *                               Int -1 (closed / stale)
 *   raw_slice(h, start, end) -> String bytes [start, end), or Int -1
 *                               (closed / stale; bounds are pre-validated
 *                               in TA)
 *   raw_close(h)             -> Int 1, idempotent (double close of the
 *                               same handle value is also 1); -1 only
 *                               for a stale handle (out of table range,
 *                               or a generation that no longer matches
 *                               its slot)
 *   raw_length(h)            -> Int length, or -1 (closed / stale)
 *
 * Error convention (docs/c-module.md §4): -1 is the hard-error signal;
 * the TA layer lifts it to Err("closed") (bounds errors are lifted to
 * Err("range") in TA before the C call is made, so -1 from slice always
 * really means a closed handle).
 *
 * Stale handles (§5 "已 close 的句柄再操作 → -1"): closed slots go on a
 * free list and are recycled, so a bare slot index would let a stale
 * handle alias the *next* buffer allocated in that slot. Handles therefore
 * carry a per-slot generation: h = slot | gen << SLOT_BITS. Recycling a
 * slot bumps its generation, so every handle minted before the close no
 * longer decodes — any operation on it returns -1 (TA: Err("closed")),
 * and in particular a double close of the OLD handle value cannot free
 * the new buffer. Double close of the SAME handle value stays idempotent
 * (generation still matches, len == -1 already => Ok(1)). Generations are
 * plain ints and never wrap in practice (wrapping needs 2^40 recycles of
 * one slot to collide with a TA i48 handle).
 *
 * slice semantics: [start, end) half-open, indices are CODEPOINTS. v1
 * strings are byte arrays and codepoint == byte (ASCII scope, same
 * convention as lib/str.c) — the two words coincide until the planned
 * unicode module.
 *
 * Lifetime (docs/c-module.md §5): explicit close, responsibility on the
 * caller. Buffer data is malloc'd (GC-invisible, stable across GC) and
 * freed only by close or process exit — there is no finalizer, so a
 * buffer whose owning actor dies leaks until VM exit (documented v1
 * leak surface, accepted by the convention).
 *
 * Thread safety: the table is global (a handle is a plain int), so every
 * operation runs under one lock. Without it, two workers allocating
 * handles at the same time raced on the free list and could hand the
 * same slot to two actors — one actor's close then freed the other's
 * storage (heap corruption) or the other actor's next use saw a closed
 * handle. The lock covers the WHOLE operation, not just the table
 * lookup: a close landing between the lookup and the copy would free the
 * buffer under us. Same pattern as net.c / random.c / timer.c.
 */

#include "ta.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define SLOT_BITS 8
#define MAX_BUFFERS (1 << SLOT_BITS)

typedef struct {
    uint8_t *data;
    int len;
    int cap;
    int gen;
} BufSlot;

static BufSlot bufs[MAX_BUFFERS];
static int next_slot = 0;
static int free_head = -1; /* linked list of freed slot indices */
static int free_next[MAX_BUFFERS];
static pthread_mutex_t g_buf_lock = PTHREAD_MUTEX_INITIALIZER;

/* Decode a handle to its slot, or NULL if invalid: out of range, a
 * generation that no longer matches (slot was closed and recycled, so
 * this stale handle must not alias the new buffer), or a freed slot
 * still awaiting reuse (len == -1).
 * Caller holds g_buf_lock. */
static BufSlot *buf_get(int64_t h) {
    if (h < 0)
        return NULL;
    int slot = (int)(h & (MAX_BUFFERS - 1));
    int gen = (int)(h >> SLOT_BITS);
    if (slot >= next_slot || bufs[slot].gen != gen)
        return NULL;
    BufSlot *b = &bufs[slot];
    if (b->len == -1)
        return NULL;
    return b;
}

/* Ensure `add` more bytes fit; double the capacity as needed.
 * Returns 1 ok, 0 OOM. Caller holds g_buf_lock. */
static int buf_ensure(BufSlot *b, int add) {
    if (b->len + add <= b->cap)
        return 1;
    int newcap = b->cap ? b->cap : 16;
    while (newcap < b->len + add)
        newcap *= 2;
    uint8_t *nd = realloc(b->data, (size_t)newcap);
    if (!nd)
        return 0;
    b->data = nd;
    b->cap = newcap;
    return 1;
}

/* Allocate a handle from the free list or the next fresh slot.
 * Recycled slots get a bumped generation so every handle minted before
 * the close decodes to NULL (stale) instead of aliasing the new buffer.
 * Caller holds g_buf_lock. */
static int64_t buf_alloc_handle(void) {
    int slot;
    if (free_head >= 0) {
        slot = free_head;
        free_head = free_next[slot];
        free_next[slot] = -1;
        bufs[slot].gen++;
    } else {
        if (next_slot >= MAX_BUFFERS)
            return -1;
        slot = next_slot++;
        bufs[slot].gen = 0;
    }
    return (int64_t)slot + ((int64_t)bufs[slot].gen << SLOT_BITS);
}

static Val buffer_raw_new(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)args;
    (void)nargs;
    pthread_mutex_lock(&g_buf_lock);
    int64_t h = buf_alloc_handle();
    Val r;
    if (h < 0) {
        r = val_int(-1);
    } else {
        int slot = (int)(h & (MAX_BUFFERS - 1)); /* h is encoded: slot | gen<<8 */
        bufs[slot].data = NULL;
        bufs[slot].len = 0;
        bufs[slot].cap = 0;
        r = val_int(h);
    }
    pthread_mutex_unlock(&g_buf_lock);
    return r;
}

static Val buffer_raw_push_str(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    pthread_mutex_lock(&g_buf_lock);
    Val r = val_int(-1);
    BufSlot *b = buf_get(val_get_int(args[0]));
    if (b && val_is_string(args[1])) {
        HeapString *hs = val_get_string(args[1]);
        if (buf_ensure(b, hs->len)) {
            memcpy(b->data + b->len, hs->data, (size_t)hs->len);
            b->len += hs->len;
            r = val_int(b->len);
        }
    }
    pthread_mutex_unlock(&g_buf_lock);
    return r;
}

static Val buffer_raw_push_byte(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    pthread_mutex_lock(&g_buf_lock);
    Val r = val_int(-1);
    BufSlot *b = buf_get(val_get_int(args[0]));
    int64_t n = val_get_int(args[1]);
    if (b && n >= 0 && n <= 255 && buf_ensure(b, 1)) {
        b->data[b->len++] = (uint8_t)n;
        r = val_int(b->len);
    }
    pthread_mutex_unlock(&g_buf_lock);
    return r;
}

/* Snapshot as a TA string. The buffer keeps its contents. b->data is
 * malloc'd (GC-invisible), so the copy is safe even though val_string
 * allocates: the source never moves. The allocation happens under the
 * lock — the GC it may trigger never calls back into this module, so
 * there is no lock cycle. */
static Val buffer_raw_to_string(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    pthread_mutex_lock(&g_buf_lock);
    Val r = val_int(-1);
    BufSlot *b = buf_get(val_get_int(args[0]));
    if (b) {
        Proc *p = tls_current_proc;
        r = val_string(p, (const char *)b->data, b->len);
    }
    pthread_mutex_unlock(&g_buf_lock);
    return r;
}

/* Substring [start, end), codepoint (== byte in v1) indices.
 * Bounds are pre-validated in TA; a bad range here still returns -1. */
static Val buffer_raw_slice(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    pthread_mutex_lock(&g_buf_lock);
    Val r = val_int(-1);
    BufSlot *b = buf_get(val_get_int(args[0]));
    int64_t start = val_get_int(args[1]);
    int64_t end = val_get_int(args[2]);
    if (b && start >= 0 && end <= b->len && start <= end) {
        Proc *p = tls_current_proc;
        r = val_string(p, (const char *)b->data + start, (int)(end - start));
    }
    pthread_mutex_unlock(&g_buf_lock);
    return r;
}

/* Release the handle's storage. Idempotent for the SAME handle value
 * (double close of a generation-matching handle also returns 1,
 * docs/c-module.md §5); a stale handle — its slot was closed and
 * recycled, so the generation no longer matches — returns -1 and can
 * never free another buffer's data. */
static Val buffer_raw_close(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)args;
    (void)nargs;
    int64_t h = val_get_int(args[0]);
    pthread_mutex_lock(&g_buf_lock);
    Val r = val_int(-1);
    if (h >= 0) {
        int slot = (int)(h & (MAX_BUFFERS - 1));
        int gen = (int)(h >> SLOT_BITS);
        if (slot < next_slot && bufs[slot].gen == gen) {
            BufSlot *b = &bufs[slot];
            if (b->len != -1) {
                free(b->data);
                b->data = NULL;
                b->len = -1;
                b->cap = 0;
                free_next[slot] = free_head;
                free_head = slot;
            }
            r = val_int(1);
        }
    }
    pthread_mutex_unlock(&g_buf_lock);
    return r;
}

static Val buffer_raw_length(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    pthread_mutex_lock(&g_buf_lock);
    Val r = val_int(-1);
    BufSlot *b = buf_get(val_get_int(args[0]));
    if (b)
        r = val_int(b->len);
    pthread_mutex_unlock(&g_buf_lock);
    return r;
}

static TaFunc buffer_funcs[] = {{"raw_new", buffer_raw_new, 0},
                                {"raw_push_str", buffer_raw_push_str, 2},
                                {"raw_push_byte", buffer_raw_push_byte, 2},
                                {"raw_to_string", buffer_raw_to_string, 1},
                                {"raw_slice", buffer_raw_slice, 3},
                                {"raw_close", buffer_raw_close, 1},
                                {"raw_length", buffer_raw_length, 1},
                                {NULL, NULL, 0}};

/* Dynamic module entry: dlsym("vm_load_self") after dlopen (main.c link_unit). */
void vm_load_self(VM *vm) { vm_register_module(vm, "buffer", buffer_funcs, 7); }