/*
 * val.c — NaN-boxing value operations for TinyActor
 *
 * Encoding (64-bit):
 *   Normal double   → stored as-is (high bit pattern != 0xFFxx)
 *   Non-double types → high 16 bits = tag, low 48 bits = payload
 *
 *   TAG_INT      0xFF00  → low 48 = sign-extended int48
 *   TAG_NIL      0xFF01  → no payload
 *   TAG_TRUE     0xFF02  → no payload
 *   TAG_FALSE    0xFF03  → no payload
 *   TAG_SYM      0xFF04  → low 32 = symbol table index
 *   TAG_PAIR     0xFF05  → low 48 = heap pointer
 *   TAG_PID      0xFF06  → low 32 = pid
 *   TAG_CLOS     0xFF07  → low 48 = heap pointer
 *   TAG_STRING   0xFF08  → low 48 = heap pointer
 *   TAG_BYTES    0xFF09  → low 48 = heap pointer
 */

#include "ta.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ============================================================
 * Internal helpers
 * ============================================================ */

/* Build a NaN-boxed value: tag in bits [63:48], payload in low 48 bits. */
static inline Val box_tag_payload(uint16_t tag, uint64_t payload) {
    return ((uint64_t)tag << 48) | (payload & 0x0000FFFFFFFFFFFFULL);
}

/* Extract the 16-bit tag from a NaN-boxed value. */
static inline uint16_t val_tag(Val v) {
    return (uint16_t)(v >> 48);
}

/* Extract the low 48 bits as unsigned. */
static inline uint64_t val_payload48(Val v) {
    return v & 0x0000FFFFFFFFFFFFULL;
}

/* Extract the low 32 bits as unsigned. */
static inline uint32_t val_payload32(Val v) {
    return (uint32_t)(v & 0xFFFFFFFFULL);
}

/* ============================================================
 * Value constructors
 * ============================================================ */

Val val_int(int64_t i) {
    /* Store as sign-extended int48 in low 48 bits.
     * Cast via union to avoid UB on signed shift. */
    union { int64_t s; uint64_t u; } u;
    u.s = i;
    return box_tag_payload(TAG_INT, u.u);
}

Val val_nil(void)    { return box_tag_payload(TAG_NIL, 0); }
Val val_true(void)   { return box_tag_payload(TAG_TRUE, 0); }
Val val_false(void)  { return box_tag_payload(TAG_FALSE, 0); }

Val val_symbol(uint32_t idx) {
    return box_tag_payload(TAG_SYM, (uint64_t)idx);
}

Val val_pid(uint32_t pid) {
    return box_tag_payload(TAG_PID, (uint64_t)pid);
}

/* ============================================================
 * Heap-allocated constructors (require process context)
 * ============================================================ */

Val val_pair(Proc *p, Val car, Val cdr) {
    HeapPair *hp = (HeapPair *)proc_heap_alloc(p, sizeof(HeapPair));
    if (!hp) return val_nil(); /* OOM — caller should trigger GC */
    hp->hdr.type  = HEAP_PAIR;
    hp->hdr.flags = 0;
    hp->car = car;
    hp->cdr = cdr;
    return box_tag_payload(TAG_PAIR, (uint64_t)(uintptr_t)hp);
}

Val val_string(Proc *p, const char *data, int len) {
    int total = sizeof(HeapString) + len + 1; /* +1 for NUL */
    HeapString *hs = (HeapString *)proc_heap_alloc(p, total);
    if (!hs) return val_nil();
    hs->hdr.type  = HEAP_STRING;
    hs->hdr.flags = 0;
    hs->len = len;
    memcpy(hs->data, data, len);
    hs->data[len] = '\0';
    return box_tag_payload(TAG_STRING, (uint64_t)(uintptr_t)hs);
}

Val val_bytes(Proc *p, const uint8_t *data, int len) {
    int total = sizeof(HeapBytes) + len;
    HeapBytes *hb = (HeapBytes *)proc_heap_alloc(p, total);
    if (!hb) return val_nil();
    hb->hdr.type  = HEAP_BYTES;
    hb->hdr.flags = 0;
    hb->len = len;
    if (data && len > 0) memcpy(hb->data, data, len);
    return box_tag_payload(TAG_BYTES, (uint64_t)(uintptr_t)hb);
}

/* ============================================================
 * Value predicates & accessors
 * ============================================================ */

int val_is_int(Val v)    { return val_tag(v) == TAG_INT; }

int64_t val_get_int(Val v) {
    union { uint64_t u; int64_t s; } u;
    u.u = val_payload48(v);
    /* Sign-extend from 48 bits */
    if (u.u & 0x800000000000ULL)
        u.u |= 0xFFFF000000000000ULL;
    return u.s;
}

int val_is_nil(Val v) { return val_tag(v) == TAG_NIL; }

int val_is_true(Val v) {
    return val_tag(v) != TAG_NIL && val_tag(v) != TAG_FALSE;
}

int val_is_pair(Val v) { return val_tag(v) == TAG_PAIR; }

Val val_get_car(Val v) {
    HeapPair *hp = (HeapPair *)(uintptr_t)val_payload48(v);
    return hp->car;
}

Val val_get_cdr(Val v) {
    HeapPair *hp = (HeapPair *)(uintptr_t)val_payload48(v);
    return hp->cdr;
}

int val_is_symbol(Val v)     { return val_tag(v) == TAG_SYM; }
uint32_t val_get_symbol(Val v) { return val_payload32(v); }

int val_is_pid(Val v)        { return val_tag(v) == TAG_PID; }
uint32_t val_get_pid(Val v)  { return val_payload32(v); }

int val_is_clos(Val v)       { return val_tag(v) == TAG_CLOS; }
int val_is_pid_type(Val v)   { return val_tag(v) == TAG_PID; }

int val_is_string(Val v)     { return val_tag(v) == TAG_STRING; }
HeapString *val_get_string(Val v) {
    return (HeapString *)(uintptr_t)val_payload48(v);
}

int val_is_bytes(Val v)      { return val_tag(v) == TAG_BYTES; }
HeapBytes *val_get_bytes(Val v) {
    return (HeapBytes *)(uintptr_t)val_payload48(v);
}

/* ============================================================
 * Deep copy — copy a value tree into a target process heap
 *
 * Immutability guarantees no cycles, so no visited table needed.
 * Heap pointers are rewritten to the target process's heap.
 * ============================================================ */

Val val_deep_copy(Proc *target, Val v) {
    uint16_t tag = val_tag(v);

    /* Immediate values — no heap data, just copy the bits */
    switch (tag) {
    case TAG_INT:
    case TAG_NIL:
    case TAG_TRUE:
    case TAG_FALSE:
    case TAG_PID:
    case TAG_SYM:
        return v;
    default:
        break;
    }

    /* Heap values — allocate on target heap and recurse */
    if (tag == TAG_PAIR) {
        HeapPair *src = (HeapPair *)(uintptr_t)val_payload48(v);
        /* Recursively copy children first so we don't lose them */
        Val car = val_deep_copy(target, src->car);
        Val cdr = val_deep_copy(target, src->cdr);
        return val_pair(target, car, cdr);
    }

    if (tag == TAG_STRING) {
        HeapString *src = (HeapString *)(uintptr_t)val_payload48(v);
        return val_string(target, src->data, src->len);
    }

    if (tag == TAG_BYTES) {
        HeapBytes *src = (HeapBytes *)(uintptr_t)val_payload48(v);
        return val_bytes(target, src->data, src->len);
    }

    if (tag == TAG_CLOS) {
        HeapClosure *src = (HeapClosure *)(uintptr_t)val_payload48(v);
        int total = sizeof(HeapClosure) + (int)(src->nfree * sizeof(Val));
        HeapClosure *dst = (HeapClosure *)proc_heap_alloc(target, total);
        if (!dst) return val_nil();
        dst->hdr.type  = HEAP_CLOS;
        dst->hdr.flags = 0;
        dst->entry     = src->entry;
        dst->nfree     = src->nfree;
        for (int i = 0; i < src->nfree; i++) {
            dst->free[i] = val_deep_copy(target, src->free[i]);
        }
                return box_tag_payload(TAG_CLOS, (uint64_t)(uintptr_t)dst);
    }

    if (tag == TAG_CLOS_ID) {
        /* Direct fn-id reference — just copy the value */
        return v;
    }

    /* Unknown tag — return nil as safe fallback */
    return val_nil();
}
