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
 * tags). Every operation runs the same preamble through buf_open:
 *
 *   1. resolve the value (val_get_resource — NULL means a forged or
 *      foreign value, impossible through typecheck but cheap to reject);
 *   2. released check → -1 (the dtor has run; every copy is dead);
 *   3. owner check → -2 (the buffer was sent to another actor; this copy
 *      is stale and the payload is now the receiver's to touch);
 *   4. work through the payload (r->res): it sits outside every actor
 *      heap, so nothing that touches it can race a GC — and inside a
 *      ccall the gate is closed anyway, so the header cannot move either.
 *
 * Signals (docs/c-module.md §4): -1 closed, -2 not owner — the TA layer
 * lifts them to Err("closed") / Err("not owner"). close is idempotent:
 * an already-released buffer still returns 1 (contract in lib/buffer.ta;
 * `released` lives on the shared payload, so this holds across every
 * stale copy too).
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

/* buf_open — resolve + validate a handle for one operation.
 * Returns 0 with the payload handed out, or the signal to relay: -1
 * invalid / released, -2 not owner. The header is read only here; the
 * caller works through the payload, which nothing ever moves. */
static int buf_open(Val v, ResourcePayload **out) {
    HeapResource *r = val_get_resource(v);
    if (r == NULL || r->res->released)
        return -1;
    if (r->owner_pid != tls_current_proc->pid)
        return -2;
    *out = r->res;
    return 0;
}

static Val buffer_raw_new(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)args;
    (void)nargs;
    /* No table limit anymore: a new payload is just a malloc (its 24-byte
     * identity record lives to VM exit by design). OOM aborts like every
     * heap constructor (val_pair & co). */
    BufState *b = (BufState *)malloc(sizeof(BufState));
    if (!b)
        abort();
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    return ta_resource_new(tls_current_proc, b, buf_dtor);
}

static Val buffer_raw_push_str(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    ResourcePayload *pl;
    int st = buf_open(args[0], &pl);
    if (st < 0)
        return val_int(st);
    if (!val_is_string(args[1]))
        return val_int(-1);
    BufState *b = (BufState *)pl->ptr;
    HeapString *hs = val_get_string(args[1]);
    if (!buf_ensure(b, hs->len))
        return val_int(-1);
    memcpy(b->data + b->len, hs->data, (size_t)hs->len);
    b->len += hs->len;
    return val_int(b->len);
}

static Val buffer_raw_push_byte(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    ResourcePayload *pl;
    int st = buf_open(args[0], &pl);
    if (st < 0)
        return val_int(st);
    int64_t n = val_get_int(args[1]);
    BufState *b = (BufState *)pl->ptr;
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
    (void)vm;
    (void)nargs;
    ResourcePayload *pl;
    int st = buf_open(args[0], &pl);
    if (st < 0)
        return val_int(st);
    BufState *b = (BufState *)pl->ptr;
    return val_string(tls_current_proc, (const char *)b->data, b->len);
}

/* Substring [start, end), codepoint (== byte in v1) indices.
 * Bounds are pre-validated in TA; a bad range here still returns -1. */
static Val buffer_raw_slice(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    ResourcePayload *pl;
    int st = buf_open(args[0], &pl);
    if (st < 0)
        return val_int(st);
    BufState *b = (BufState *)pl->ptr;
    int64_t start = val_get_int(args[1]);
    int64_t end = val_get_int(args[2]);
    if (start >= 0 && end <= b->len && start <= end)
        return val_string(tls_current_proc, (const char *)b->data + start, (int)(end - start));
    return val_int(-1);
}

/* Release the buffer's storage. Idempotent: an already-released buffer
 * returns 1 again (`released` is shared on the payload, so every stale
 * copy hits the same no-op). A stale copy — sent away, owned by another
 * actor — returns -2 and can never free the receiver's storage. */
static Val buffer_raw_close(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    HeapResource *r = val_get_resource(args[0]);
    if (r == NULL)
        return val_int(-1);
    if (r->owner_pid != tls_current_proc->pid)
        return val_int(-2);
    if (!r->res->released)
        ta_resource_release(r);
    return val_int(1);
}

static Val buffer_raw_length(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    ResourcePayload *pl;
    int st = buf_open(args[0], &pl);
    if (st < 0)
        return val_int(st);
    return val_int(((BufState *)pl->ptr)->len);
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