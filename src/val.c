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

/* Constructors cannot fail: proc_heap_alloc aborts on arena exhaustion
 * instead of handing back a half-built value. */
Val val_pair(Proc *p, Val car, Val cdr) {
    HeapPair *hp = (HeapPair *)proc_heap_alloc(p, sizeof(HeapPair));
    hp->hdr.type = HEAP_PAIR;
    hp->hdr.flags = 0;
    hp->car = car;
    hp->cdr = cdr;
    return box_tag_payload(TAG_PAIR, (uint64_t)(uintptr_t)hp);
}

Val val_string(Proc *p, const char *data, int len) {
    int total = sizeof(HeapString) + len + 1; /* +1 for NUL */
    HeapString *hs = (HeapString *)proc_heap_alloc(p, total);
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
     * float are valid inputs; any other type degrades to 0.0 defensively.
     * Arithmetic (OP_ADD/SUB/MUL/DIV) relies on that degradation to mirror
     * the golden reference (a non-int operand becomes 0.0). Comparison
     * opcodes do NOT: they take this path only when one operand is a float and
     * the other is numeric (val_is_num in vm.c), so a non-numeric never
     * reaches here — otherwise
     * `"str" == 0.0` would be true. */
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
 * Immutability guarantees no cycles, so no visited table is needed. The
 * explicit work stacks are important: messages and captured values can be
 * much deeper than the native C stack.
 * ============================================================ */

typedef struct {
    int kind;
    Val value;
} CopyTask;

#define COPY_VALUE 0
#define COPY_PAIR 1
#define COPY_CLOSURE 2

static void copy_grow(void **items, int *cap, int count, size_t size) {
    if (count < *cap)
        return;
    int next = *cap ? *cap * 2 : 64;
    void *grown = realloc(*items, (size_t)next * size);
    if (!grown)
        abort();
    *items = grown;
    *cap = next;
}

Val val_deep_copy(Proc *target, Val root) {
    /* The worklist (tasks[]) and results[] keep Vals of the target heap in
     * malloc'd / C-local storage, off the TA stack, so no collection may run
     * while the tree is rebuilt: close the gate. Left un-drained on the way
     * out — the result is still unrooted; the caller roots it (OP_RECV* push
     * it, or the next gate-open allocation) and the pending request is
     * honoured there. */
    proc_gc_enter(target);
    CopyTask *tasks = NULL;
    Val *results = NULL;
    int task_count = 0, task_cap = 0;
    int result_count = 0, result_cap = 0;

    copy_grow((void **)&tasks, &task_cap, task_count, sizeof(*tasks));
    tasks[task_count++] = (CopyTask){COPY_VALUE, root};

    while (task_count > 0) {
        CopyTask task = tasks[--task_count];
        uint16_t tag = val_tag(task.value);

        if (task.kind == COPY_PAIR) {
            Val cdr = results[--result_count];
            Val car = results[--result_count];
            copy_grow((void **)&results, &result_cap, result_count, sizeof(*results));
            results[result_count++] = val_pair(target, car, cdr);
            continue;
        }
        if (task.kind == COPY_CLOSURE) {
            HeapClosure *src = (HeapClosure *)(uintptr_t)val_payload48(task.value);
            int total = sizeof(HeapClosure) + (int)(src->nfree * sizeof(Val));
            HeapClosure *dst = (HeapClosure *)proc_heap_alloc(target, total);
            dst->hdr.type = HEAP_CLOS;
            dst->hdr.flags = 0;
            dst->entry = src->entry;
            dst->nfree = src->nfree;
            for (int i = src->nfree - 1; i >= 0; i--)
                dst->free[i] = results[--result_count];
            copy_grow((void **)&results, &result_cap, result_count, sizeof(*results));
            results[result_count++] = box_tag_payload(TAG_CLOS, (uint64_t)(uintptr_t)dst);
            continue;
        }

        if (val_is_float(task.value) || tag == TAG_INT || tag == TAG_NIL || tag == TAG_TRUE ||
            tag == TAG_FALSE || tag == TAG_PID || tag == TAG_SYM || tag == TAG_CLOS_ID) {
            copy_grow((void **)&results, &result_cap, result_count, sizeof(*results));
            results[result_count++] = task.value;
        } else if (tag == TAG_PAIR) {
            HeapPair *src = (HeapPair *)(uintptr_t)val_payload48(task.value);
            copy_grow((void **)&tasks, &task_cap, task_count, sizeof(*tasks));
            tasks[task_count++] = (CopyTask){COPY_PAIR, task.value};
            copy_grow((void **)&tasks, &task_cap, task_count, sizeof(*tasks));
            tasks[task_count++] = (CopyTask){COPY_VALUE, src->cdr};
            copy_grow((void **)&tasks, &task_cap, task_count, sizeof(*tasks));
            tasks[task_count++] = (CopyTask){COPY_VALUE, src->car};
        } else if (tag == TAG_STRING) {
            HeapString *src = (HeapString *)(uintptr_t)val_payload48(task.value);
            copy_grow((void **)&results, &result_cap, result_count, sizeof(*results));
            results[result_count++] = val_string(target, src->data, src->len);
        } else if (tag == TAG_BYTES) {
            HeapBytes *src = (HeapBytes *)(uintptr_t)val_payload48(task.value);
            copy_grow((void **)&results, &result_cap, result_count, sizeof(*results));
            results[result_count++] = val_bytes(target, src->data, src->len);
        } else if (tag == TAG_CLOS) {
            HeapClosure *src = (HeapClosure *)(uintptr_t)val_payload48(task.value);
            copy_grow((void **)&tasks, &task_cap, task_count, sizeof(*tasks));
            tasks[task_count++] = (CopyTask){COPY_CLOSURE, task.value};
            for (int i = src->nfree - 1; i >= 0; i--) {
                copy_grow((void **)&tasks, &task_cap, task_count, sizeof(*tasks));
                tasks[task_count++] = (CopyTask){COPY_VALUE, src->free[i]};
            }
        } else {
            copy_grow((void **)&results, &result_cap, result_count, sizeof(*results));
            results[result_count++] = val_nil();
        }
    }

    Val result = results[0];
    free(tasks);
    free(results);
    proc_gc_leave(target);
    return result;
}
