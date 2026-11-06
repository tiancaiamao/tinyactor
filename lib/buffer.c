/*
 * lib/buffer.c — buffer module for TinyActor (built as lib/buffer.dylib / .so)
 *
 * Mutable byte buffer: the first module on the opaque Resource value
 * (docs/c-resource-design.md, issue #401). The slot table, the generation
 * handles and the module lock are gone — the state lives in a malloc'd
 * BufState owned by the shared ResourcePayload; the actor-heap header
 * (one per holder, GC-moved) carries only the ownership metadata.
 *
 * The TA side wraps the payload value in `type Buffer { Buf(Resource) }`
 * (the constructor is still the capability; the C side never validates
 * tags). Every operation runs the same preamble through ta_resource_open
 * (src/resource.c — the single definition of resolve + owner check +
 * released check, shared with the other resource modules):
 *
 *   1. -2 when the buffer was sent to another actor (this copy is stale
 *      and the payload is now the receiver's to touch — and the receiver's
 *      to free, so this copy must not look inside it);
 *   2. -1 when the handle was never a resource or is already released
 *      (res == NULL: the dtor has run and the payload is already freed);
 *   3. work through the payload (r->res): it sits outside every actor
 *      heap, so nothing that touches it can race a GC — and inside a
 *      ccall the gate is closed anyway, so the header cannot move either.
 *
 * Signals (docs/c-module.md §4): -1 closed, -2 not owner — the TA layer
 * lifts them to Err("closed") / Err("not owner"). close is idempotent:
 * an already-released buffer still returns 1 (contract in lib/buffer.ta;
 * release clears this header's payload pointer, so every alias of it sees
 * the same released state).
 *
 * dtor = buf_dtor: free(data) + free(state). Plain frees with no VM
 * entry, so it is legal in all four release contexts (close / GC tail /
 * proc_die / vm_free, docs/c-resource-design.md §6.1).
 *
 * slice semantics: [start, end) half-open, indices are CODEPOINTS. v1
 * strings are byte arrays and codepoint == byte (ASCII scope, same
 * convention as lib/str.c) — the two words coincide until the planned
 * unicode module.
 */

#include "ta.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *data;
    int len;
    int cap;
} BufState;

static void buf_dtor(void *p) {
    BufState *b = (BufState *)p;
    free(b->data);
    free(b);
}

/* Ensure `add` more bytes fit; double the capacity as needed.
 * Returns 1 ok, 0 OOM. */
static int buf_ensure(BufState *b, int add) {
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

static Val buffer_raw_new(VM *vm, Val *args, int nargs) {
    /* No table limit anymore: a new payload is just a malloc, freed again by
     * buffer_raw_close (ta_resource_release). OOM aborts like every heap
     * constructor (val_pair & co). */
    BufState *b = (BufState *)malloc(sizeof(BufState));
    if (!b)
        abort();
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    return ta_resource_new(tls_current_proc, b, buf_dtor);
}

static Val buffer_raw_push_str(VM *vm, Val *args, int nargs) {
    HeapResource *r;
    int st = ta_resource_open(args[0], &r, -1);
    if (st < 0)
        return val_int(st);
    if (!val_is_string(args[1]))
        return val_int(-1);
    BufState *b = (BufState *)r->res->ptr;
    HeapString *hs = val_get_string(args[1]);
    if (!buf_ensure(b, hs->len))
        return val_int(-1);
    memcpy(b->data + b->len, hs->data, (size_t)hs->len);
    b->len += hs->len;
    return val_int(b->len);
}

static Val buffer_raw_push_byte(VM *vm, Val *args, int nargs) {
    HeapResource *r;
    int st = ta_resource_open(args[0], &r, -1);
    if (st < 0)
        return val_int(st);
    int64_t n = val_get_int(args[1]);
    BufState *b = (BufState *)r->res->ptr;
    if (n >= 0 && n <= 255 && buf_ensure(b, 1)) {
        b->data[b->len++] = (uint8_t)n;
        return val_int(b->len);
    }
    return val_int(-1);
}

/* Snapshot as a TA string. The buffer keeps its contents. b->data is
 * malloc'd (GC-invisible), so the copy is safe even though val_string
 * allocates: the source never moves. */
static Val buffer_raw_to_string(VM *vm, Val *args, int nargs) {
    HeapResource *r;
    int st = ta_resource_open(args[0], &r, -1);
    if (st < 0)
        return val_int(st);
    BufState *b = (BufState *)r->res->ptr;
    return val_string(tls_current_proc, (const char *)b->data, b->len);
}

/* Substring [start, end), codepoint (== byte in v1) indices.
 * Bounds are pre-validated in TA; a bad range here still returns -1. */
static Val buffer_raw_slice(VM *vm, Val *args, int nargs) {
    HeapResource *r;
    int st = ta_resource_open(args[0], &r, -1);
    if (st < 0)
        return val_int(st);
    BufState *b = (BufState *)r->res->ptr;
    int64_t start = val_get_int(args[1]);
    int64_t end = val_get_int(args[2]);
    if (start >= 0 && end <= b->len && start <= end)
        return val_string(tls_current_proc, (const char *)b->data + start, (int)(end - start));
    return val_int(-1);
}

/* Release the buffer's storage. Idempotent: an already-released buffer
 * returns 1 again (the release clears the header's payload pointer, so every
 * alias of this header hits the same no-op). A stale copy — sent away, owned
 * by another actor — returns -2 and can never free the receiver's storage. */
static Val buffer_raw_close(VM *vm, Val *args, int nargs) {
    HeapResource *r;
    /* closed_code = 1: an already-released buffer is a successful close. */
    int st = ta_resource_open(args[0], &r, 1);
    if (st != 0)
        return val_int(st);
    ta_resource_release(r); /* no-op when already released */
    return val_int(1);
}

static Val buffer_raw_length(VM *vm, Val *args, int nargs) {
    HeapResource *r;
    int st = ta_resource_open(args[0], &r, -1);
    if (st < 0)
        return val_int(st);
    return val_int(((BufState *)r->res->ptr)->len);
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