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
 *   TAG_CLOS_ID  0xFF0A  → low 32 = direct fn_id
 *
 * Float discrimination: a value is a float iff its TOP BYTE (bits 63:56)
 * is not 0xFF — the tag region lives in bits 63:48 with the top byte 0xFF.
 * This matches the "normal double stored as-is" convention: any double whose
 * sign+exponent byte is not 0xFF counts as a float.
 *
 * COLLISION NOTE: -Infinity (0xFFF0...0) and -NaN (0xFFF8...0) have a top
 * byte of 0xFF and are therefore misclassified as tagged values. The baseline
 * never constructs NaN; division-by-zero of positive operands yields +Inf
 * (0x7FF0...0, top byte 0x7F — correctly a float), which is the only inf the
 * baseline produces. -Inf can only arise from negative/zero division, which
 * is out of scope for now.
 */

#include "ta.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * Internal helpers
 * ============================================================ */

/* Build a NaN-boxed value: tag in bits [63:48], payload in low 48 bits. */
static inline Val box_tag_payload(uint16_t tag, uint64_t payload) {
    return ((uint64_t)tag << 48) | (payload & 0x0000FFFFFFFFFFFFULL);
}

/* Extract the low 48 bits as unsigned. */
static inline uint64_t val_payload48(Val v) { return v & 0x0000FFFFFFFFFFFFULL; }

/* Extract the low 32 bits as unsigned. */
static inline uint32_t val_payload32(Val v) { return (uint32_t)(v & 0xFFFFFFFFULL); }

/* ============================================================
 * Value constructors
 * ============================================================ */

Val val_int(int64_t i) {
    /* Store as sign-extended int48 in low 48 bits.
     * Cast via union to avoid UB on signed shift. */
    union {
        int64_t s;
        uint64_t u;
    } u;
    u.s = i;
    return box_tag_payload(TAG_INT, u.u);
}

Val val_float(double d) {
    /* Normal doubles are stored as-is: the bit pattern is the value itself.
     * NaN-boxing leaves it untouched, so the float discrimination rule is
     * simply "top byte != 0xFF" (see the header comment for the -NaN/-Inf
     * collision). */
    union {
        double d;
        uint64_t u;
    } u;
    u.d = d;
    return u.u;
}

Val val_from_double(double d) {
    /* Deliberately NEVER narrows back to int: any arithmetic result that
     * involved a float stays a float, so `1.0 + 2` yields 3.0, not 3.
     * This keeps mixed-type results unambiguous. */
    return val_float(d);
}

Val val_nil(void) { return box_tag_payload(TAG_NIL, 0); }
Val val_true(void) { return box_tag_payload(TAG_TRUE, 0); }
Val val_false(void) { return box_tag_payload(TAG_FALSE, 0); }

Val val_symbol(uint32_t idx) { return box_tag_payload(TAG_SYM, (uint64_t)idx); }

Val val_pid(uint32_t pid) { return box_tag_payload(TAG_PID, (uint64_t)pid); }

/* ============================================================
 * Heap-allocated constructors (require process context)
 * ============================================================ */

Val val_pair(Proc *p, Val car, Val cdr) {
    gc_root_push(p, car);
    gc_root_push(p, cdr);
    HeapPair *hp = (HeapPair *)proc_heap_alloc(p, sizeof(HeapPair));
    cdr = gc_root_pop(p); /* cdr */
    car = gc_root_pop(p); /* car */
    if (!hp)
        return val_nil(); /* OOM — caller should trigger GC */
    hp->hdr.type = HEAP_PAIR;
    hp->hdr.flags = 0;
    hp->car = car;
    hp->cdr = cdr;
    return box_tag_payload(TAG_PAIR, (uint64_t)(uintptr_t)hp);
}

Val val_string(Proc *p, const char *data, int len) {
    int total = sizeof(HeapString) + len + 1; /* +1 for NUL */
    HeapString *hs = (HeapString *)proc_heap_alloc(p, total);
    if (!hs)
        return val_nil();
    hs->hdr.type = HEAP_STRING;
    hs->hdr.flags = 0;
    hs->len = len;
    memcpy(hs->data, data, len);
    hs->data[len] = '\0';
    return box_tag_payload(TAG_STRING, (uint64_t)(uintptr_t)hs);
}

Val val_bytes(Proc *p, const uint8_t *data, int len) {
    int total = sizeof(HeapBytes) + len;
    HeapBytes *hb = (HeapBytes *)proc_heap_alloc(p, total);
    if (!hb)
        return val_nil();
    hb->hdr.type = HEAP_BYTES;
    hb->hdr.flags = 0;
    hb->len = len;
    if (data && len > 0)
        memcpy(hb->data, data, len);
    return box_tag_payload(TAG_BYTES, (uint64_t)(uintptr_t)hb);
}

/* ============================================================
 * Value predicates & accessors
 * ============================================================ */

int val_is_int(Val v) { return val_tag(v) == TAG_INT; }

int64_t val_get_int(Val v) {
    union {
        uint64_t u;
        int64_t s;
    } u;
    u.u = val_payload48(v);
    /* Sign-extend from 48 bits */
    if (u.u & 0x800000000000ULL)
        u.u |= 0xFFFF000000000000ULL;
    return u.s;
}

int val_is_float(Val v) {
    /* Float discrimination: top byte != 0xFF. All NaN-boxed tags have top
     * byte 0xFF, so this is exactly "not a tagged value". -NaN/-Inf collide
     * (top byte 0xFF) — documented in the header; the baseline never
     * constructs NaN and only produces +Inf from division by zero. */
    return ((v >> 56) & 0xFF) != 0xFF;
}

double val_get_float(Val v) {
    union {
        uint64_t u;
        double d;
    } u;
    u.u = v;
    return u.d;
}

double val_to_double(Val v) {
    /* int → double widening for mixed arithmetic/comparison. Only int and
     * float are valid inputs (typechecker prevents anything else); any other
     * type degrades to 0.0 defensively. */
    if (val_is_float(v))
        return val_get_float(v);
    if (val_is_int(v))
        return (double)val_get_int(v);
    return 0.0;
}

int val_is_nil(Val v) { return val_tag(v) == TAG_NIL; }

int val_is_true(Val v) { return val_tag(v) != TAG_NIL && val_tag(v) != TAG_FALSE; }

int val_is_pair(Val v) { return val_tag(v) == TAG_PAIR; }

Val val_get_car(Val v) {
    HeapPair *hp = (HeapPair *)(uintptr_t)val_payload48(v);
    return hp->car;
}

Val val_get_cdr(Val v) {
    HeapPair *hp = (HeapPair *)(uintptr_t)val_payload48(v);
    return hp->cdr;
}

int val_is_symbol(Val v) { return val_tag(v) == TAG_SYM; }
uint32_t val_get_symbol(Val v) { return val_payload32(v); }

int val_is_pid(Val v) { return val_tag(v) == TAG_PID; }
uint32_t val_get_pid(Val v) { return val_payload32(v); }

int val_is_clos(Val v) { return val_tag(v) == TAG_CLOS; }

int val_is_string(Val v) { return val_tag(v) == TAG_STRING; }
HeapString *val_get_string(Val v) { return (HeapString *)(uintptr_t)val_payload48(v); }

int val_is_bytes(Val v) { return val_tag(v) == TAG_BYTES; }
HeapBytes *val_get_bytes(Val v) { return (HeapBytes *)(uintptr_t)val_payload48(v); }

/* ============================================================
 * Deep copy — copy a value tree into a target process heap
 *
 * Immutability guarantees no cycles, so no visited table needed.
 * Heap pointers are rewritten to the target process's heap.
 * ============================================================ */

Val val_deep_copy(Proc *target, Val v) {
    uint16_t tag = val_tag(v);

    /* Floats are immediate values (raw double bits) — copy as-is. Must be
     * checked before the tag switch: a float's top 16 bits are the double's
     * sign+exponent, which matches no tag and would fall through to the
     * unknown-tag fallback (nil) below. */
    if (val_is_float(v))
        return v;

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
        if (!dst)
            return val_nil();
        dst->hdr.type = HEAP_CLOS;
        dst->hdr.flags = 0;
        dst->entry = src->entry;
        dst->nfree = src->nfree;
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
