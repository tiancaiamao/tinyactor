/*
 * gc.c — Semispace copying garbage collector for TinyActor
 */

#include "ta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLAG_FORWARDED 0x01

#ifdef GC_DEBUG
#include <assert.h>
#define GC_ASSERT(x) assert(x)
#else
#define GC_ASSERT(x) ((void)0)
#endif

static int obj_size(void *obj) {
    HeapHeader *h = (HeapHeader *)obj;
    switch (h->type) {
    case HEAP_PAIR:
        return sizeof(HeapPair);
    case HEAP_CLOS:
        return sizeof(HeapClosure) + ((HeapClosure *)obj)->nfree * (int)sizeof(Val);
    case HEAP_STRING:
        return sizeof(HeapString) + ((HeapString *)obj)->len + 1;
    case HEAP_BYTES:
        return sizeof(HeapBytes) + ((HeapBytes *)obj)->len;
    default:
        GC_ASSERT(h->type == HEAP_PAIR || h->type == HEAP_CLOS || h->type == HEAP_STRING ||
                  h->type == HEAP_BYTES);
        fprintf(stderr, "gc: unknown heap type %d\n", h->type);
        abort();
    }
}

static int in_fromspace(Proc *p, void *ptr) {
    return (uint8_t *)ptr >= p->mem && (uint8_t *)ptr < p->mem + p->heap_ptr;
}

/* ============================================================
 * Heap integrity verifier (issue #109, TA_VERIFY_HEAP=1).
 *
 * p->mem is one malloc'd blob, so a wild write that stays inside the
 * heap is invisible to ASan — corruption only surfaces far away as a
 * wrong .tabc or a bogus typecheck error. Every allocated object in
 * [0, heap_ptr) must be structurally valid at gc_collect entry: the
 * mutator fully initializes an object before the bump pointer moves
 * past it, and no GC can run mid-initialization. Checking at the
 * first GC after a smash localizes the crime (heap offset, field,
 * observed value, gc_count) instead of letting it surface as an
 * end-of-run output diff.
 *
 * Checked per object: legal header type; PAIR car/cdr carry a legal
 * TAG_* (immediates are legal children — only the tag set is checked,
 * not the target: cross-heap references are legal); CLOS nfree in
 * range and legal free[i] tags; STRING len in range and NUL
 * termination; BYTES len in range. Dead (unreachable) objects are
 * checked too — they were valid when last copied and are only cleared
 * wholesale on space swap, so a structurally invalid object is a
 * smash regardless of reachability.
 * ============================================================ */

static int verify_heap_enabled(void) {
    static int enabled = -1; /* benign race: all writers store the same value */
    if (enabled < 0) {
        const char *e = getenv("TA_VERIFY_HEAP");
        enabled = (e != NULL && *e != '\0' && strcmp(e, "0") != 0);
    }
    return enabled;
}

static int val_legal_tag(uint64_t tag) {
    return tag == TAG_INT || tag == TAG_NIL || tag == TAG_TRUE || tag == TAG_FALSE ||
           tag == TAG_SYM || tag == TAG_PAIR || tag == TAG_PID || tag == TAG_CLOS ||
           tag == TAG_STRING || tag == TAG_BYTES || tag == TAG_CLOS_ID;
}

static void heap_verify_fail(Proc *p, int off, const char *what, unsigned long long got) {
    fprintf(stderr,
            "gc verify: %s at heap offset %d (got 0x%llx); pid %d gc_count=%d heap_ptr=%d — "
            "heap smash, aborting (issue #109; unset TA_VERIFY_HEAP to disable)\n",
            what, off, got, p->pid, p->gc_count, p->heap_ptr);
    abort();
}

// Audit every stack slot and gc_root for a legal Val tag (issue #109):
// a slot holding tag 0x4070 etc. means a garbage value entered a live
// location between GCs — report the exact slot and abort.
static void gc_audit_roots(Proc *p) {
    Val *stack = (Val *)(p->mem + p->mem_size);
    for (int i = p->sp; i < 0; i++) {
        if (!val_legal_tag(val_tag(stack[i]))) {
            fprintf(stderr,
                    "gc audit: STACK slot %d holds illegal tag=0x%04llx "
                    "(raw=0x%016llx); pid %d gc_count=%d sp=%d fp=%d "
                    "heap_ptr=%d pc=%d fn=%s -- aborting\n",
                    i, (unsigned long long)val_tag(stack[i]), (unsigned long long)stack[i], p->pid,
                    p->gc_count, p->sp, p->fp, p->heap_ptr, p->pc,
                    vm_fn_name(p, p->pc) ? vm_fn_name(p, p->pc) : "?");
            int lo = p->sp - 2;
            if (lo < -24)
                lo = -24;
            for (int j = 0; j > lo; j--)
                fprintf(stderr, "  slot[%4d] raw=0x%016llx tag=0x%04llx\n", j,
                        (unsigned long long)stack[j], (unsigned long long)val_tag(stack[j]));
            abort();
        }
    }
    for (int i = 0; i < p->gc_root_count; i++) {
        if (!val_legal_tag(val_tag(p->gc_roots[i]))) {
            fprintf(stderr,
                    "gc audit: ROOT %d holds illegal tag=0x%04llx "
                    "(raw=0x%016llx); pid %d gc_count=%d -- aborting\n",
                    i, (unsigned long long)val_tag(p->gc_roots[i]),
                    (unsigned long long)p->gc_roots[i], p->pid, p->gc_count);
            abort();
        }
    }
}

static void gc_verify_heap(Proc *p) {
    int off = 0;
    while (off < p->heap_ptr) {
        HeapHeader *h = (HeapHeader *)(p->mem + off);
        int sz;
        if (h->type != HEAP_PAIR && h->type != HEAP_CLOS && h->type != HEAP_STRING &&
            h->type != HEAP_BYTES) {
            heap_verify_fail(p, off, "illegal object type", h->type);
            return;
        }
        switch (h->type) {
        case HEAP_PAIR: {
            HeapPair *hp = (HeapPair *)h;
            if (!val_legal_tag(val_tag(hp->car)))
                heap_verify_fail(p, off, "pair.car illegal tag", hp->car);
            if (!val_legal_tag(val_tag(hp->cdr)))
                heap_verify_fail(p, off, "pair.cdr illegal tag", hp->cdr);
            sz = (int)sizeof(HeapPair);
            break;
        }
        case HEAP_CLOS: {
            HeapClosure *hc = (HeapClosure *)h;
            if (hc->nfree < 0 || hc->nfree > (p->heap_ptr - off) / (int)sizeof(Val))
                heap_verify_fail(p, off, "closure nfree out of range",
                                 (unsigned long long)hc->nfree);
            for (int i = 0; i < hc->nfree; i++)
                if (!val_legal_tag(val_tag(hc->free[i])))
                    heap_verify_fail(p, off, "closure free[] illegal tag", hc->free[i]);
            sz = (int)sizeof(HeapClosure) + hc->nfree * (int)sizeof(Val);
            break;
        }
        case HEAP_STRING: {
            HeapString *hs = (HeapString *)h;
            if (hs->len < 0 || hs->len > p->heap_ptr - off)
                heap_verify_fail(p, off, "string len out of range", (unsigned long long)hs->len);
            if (hs->data[hs->len] != '\0')
                heap_verify_fail(p, off, "string not NUL-terminated",
                                 (unsigned long long)(uint8_t)hs->data[hs->len]);
            sz = (int)sizeof(HeapString) + hs->len + 1;
            break;
        }
        default: {
            HeapBytes *hb = (HeapBytes *)h;
            if (hb->len < 0 || hb->len > p->heap_ptr - off)
                heap_verify_fail(p, off, "bytes len out of range", (unsigned long long)hb->len);
            sz = (int)sizeof(HeapBytes) + hb->len;
            break;
        }
        }
        off += (sz + 7) & ~7;
    }
}

/* Adjust a Val's pointer payload by delta if it's a heap-pointer type
 * and the pointer falls within the given memory range [lo, hi). */
static void fixup_val_in_range(Val *v, intptr_t delta, uintptr_t lo, uintptr_t hi) {
    uint64_t tag = val_tag(*v);
    if (tag == TAG_PAIR || tag == TAG_CLOS || tag == TAG_STRING || tag == TAG_BYTES) {
        uintptr_t ptr = (uintptr_t)(*v & 0x0000FFFFFFFFFFFFULL);
        if (ptr >= lo && ptr < hi) {
            ptr += delta;
            *v = (*v & 0xFFFF000000000000ULL) | (uint64_t)(ptr & 0x0000FFFFFFFFFFFFULL);
        }
    }
}

/* Walk objects in a buffer [0, limit) and fixup all child Val pointers. */
static void fixup_buffer(uint8_t *buf, int limit, intptr_t delta, uintptr_t old_lo,
                         uintptr_t old_hi) {
    int off = 0;
    while (off < limit) {
        HeapHeader *h = (HeapHeader *)(buf + off);
        int sz;
        switch (h->type) {
        case HEAP_PAIR: {
            HeapPair *hp = (HeapPair *)h;
            fixup_val_in_range(&hp->car, delta, old_lo, old_hi);
            fixup_val_in_range(&hp->cdr, delta, old_lo, old_hi);
            sz = sizeof(HeapPair);
            break;
        }
        case HEAP_CLOS: {
            HeapClosure *hc = (HeapClosure *)h;
            for (int i = 0; i < hc->nfree; i++)
                fixup_val_in_range(&hc->free[i], delta, old_lo, old_hi);
            sz = sizeof(HeapClosure) + hc->nfree * (int)sizeof(Val);
            break;
        }
        case HEAP_STRING:
            sz = sizeof(HeapString) + ((HeapString *)h)->len + 1;
            break;
        case HEAP_BYTES:
            sz = sizeof(HeapBytes) + ((HeapBytes *)h)->len;
            break;
        default:
            fprintf(stderr, "gc_fixup: unknown heap type %d at offset %d\n", h->type, off);
            return;
        }
        off += (sz + 7) & ~7;
    }
}

static void *gc_copy_obj(Proc *p, void *obj) {
    HeapHeader *h = (HeapHeader *)obj;
    GC_ASSERT((uint8_t *)obj >= p->mem && (uint8_t *)obj < p->mem + p->heap_ptr);
    if (h->flags & FLAG_FORWARDED) {
        /* forwarding pointer stored after header */
        void *fwd;
        memcpy(&fwd, (uint8_t *)obj + sizeof(HeapHeader), sizeof(void *));
        return fwd;
    }
    int sz = obj_size(obj);
    sz = (sz + 7) & ~7; /* align to 8 */
    void *new_obj = p->gc_to + p->gc_to_size;
    memcpy(new_obj, obj, sz);
    p->gc_to_size += sz;
    /* leave forwarding pointer */
    h->flags |= FLAG_FORWARDED;
    memcpy((uint8_t *)obj + sizeof(HeapHeader), &new_obj, sizeof(void *));
    GC_ASSERT((uint8_t *)new_obj >= p->gc_to && (uint8_t *)new_obj < p->gc_to + p->gc_to_size);
    return new_obj;
}

static void gc_copy_val(Proc *p, Val *v) {
    uint64_t tag = val_tag(*v);
    if (tag != TAG_PAIR && tag != TAG_CLOS && tag != TAG_STRING && tag != TAG_BYTES)
        return; /* immediate value, no heap pointer */
    void *ptr = (void *)(uintptr_t)(*v & 0x0000FFFFFFFFFFFFULL);
    if (!in_fromspace(p, ptr))
        return;
    void *new_ptr = gc_copy_obj(p, ptr);
    *v = (*v & 0xFFFF000000000000ULL) | (uint64_t)(uintptr_t)new_ptr;
}

static void gc_scan_tospace(Proc *p) {
    int scan = 0;
    while (scan < p->gc_to_size) {
        GC_ASSERT(scan <= p->gc_to_size);
        HeapHeader *h = (HeapHeader *)(p->gc_to + scan);
        GC_ASSERT(h->type == HEAP_PAIR || h->type == HEAP_CLOS || h->type == HEAP_STRING ||
                  h->type == HEAP_BYTES);
        switch (h->type) {
        case HEAP_PAIR: {
            HeapPair *hp = (HeapPair *)h;
            gc_copy_val(p, &hp->car);
            gc_copy_val(p, &hp->cdr);
            break;
        }
        case HEAP_CLOS: {
            HeapClosure *hc = (HeapClosure *)h;
            for (int i = 0; i < hc->nfree; i++)
                gc_copy_val(p, &hc->free[i]);
            break;
        }
            /* HEAP_STRING and HEAP_BYTES have no child Val refs */
        }
        int sz = obj_size(h);
        scan += (sz + 7) & ~7;
    }
}

void gc_collect(Proc *p) {
    if (p->mem == NULL)
        return; /* idle proc with no heap — nothing to collect */
    GC_ASSERT(p->heap_ptr >= 0 && p->heap_ptr <= p->mem_size);
    p->gc_count++;
    if (verify_heap_enabled())
        gc_verify_heap(p);
    if (verify_heap_enabled())
        gc_audit_roots(p);
    /* Ensure gc_to is allocated for this GC cycle. It is lazily
     * allocated to match mem_size. After the swap below, gc_to will
     * point to the old fromspace and remain available for next GC.
     * Idle actors that never trigger GC never pay this cost. */
    if (p->gc_to == NULL) {
        p->gc_to = calloc(1, p->mem_size);
    }
    p->gc_to_size = 0;

    int orig_mem_size = p->mem_size; /* stack data lives relative to original size */

    /* Scan stack roots */
    Val *stack = (Val *)(p->mem + p->mem_size);
    for (int i = p->sp; i < 0; i++) {
        gc_copy_val(p, &stack[i]);
    }

    /* Scan gc_roots */
    for (int i = 0; i < p->gc_root_count; i++) {
        gc_copy_val(p, &p->gc_roots[i]);
    }

    /* Scan tospace (fix internal refs) */
    gc_scan_tospace(p);

    /* Pre-swap assertions: live data must fit in heap */
    GC_ASSERT(p->gc_to_size >= 0);
    GC_ASSERT(p->gc_to_size <= p->mem_size);

    /* If compacted live data would overlap the stack area after swap,
     * grow both buffers before swapping.  After gc_copy_val, all root
     * pointers (stack, gc_roots) and tospace-internal pointers reference
     * gc_to.  If realloc moves gc_to, those pointers must be fixed. */
    int stack_start = p->mem_size + p->sp * (int)sizeof(Val);
    if (p->gc_to_size > stack_start) {
        int new_size = p->mem_size * 2;
        uint8_t *old_gc = p->gc_to;
        uint8_t *new_gc = realloc(p->gc_to, new_size);
        uint8_t *new_mem = realloc(p->mem, new_size);
        if (new_gc && new_mem) {
            intptr_t delta = (intptr_t)(new_gc - old_gc);
            p->gc_to = new_gc;
            p->mem = new_mem;
            p->mem_size = new_size;
            if (delta != 0) {
                uintptr_t old_lo = (uintptr_t)(p->gc_to - delta);
                uintptr_t old_hi = old_lo + p->gc_to_size;
                /* Fix tospace internal pointers */
                fixup_buffer(p->gc_to, p->gc_to_size, delta, old_lo, old_hi);
                /* Fix stack roots in fromspace (they point into gc_to) */
                Val *stk = (Val *)(p->mem + orig_mem_size);
                for (int i = p->sp; i < 0; i++)
                    fixup_val_in_range(&stk[i], delta, old_lo, old_hi);
                /* Fix gc_roots */
                for (int i = 0; i < p->gc_root_count; i++)
                    fixup_val_in_range(&p->gc_roots[i], delta, old_lo, old_hi);
            }
        }
    }

    /* Swap from/to */
    int saved_gc_to_size = p->gc_to_size;
    (void)saved_gc_to_size; /* used only in GC_ASSERT; suppress unused warning */
    uint8_t *old_mem = p->mem;
    int old_mem_size = p->mem_size;
    p->mem = p->gc_to;
    p->heap_ptr = p->gc_to_size;
    p->mem_size = old_mem_size;
    p->gc_to = old_mem;
    p->gc_to_size = 0;

    /* Post-swap assertions */
    GC_ASSERT(p->heap_ptr == saved_gc_to_size);
    GC_ASSERT(p->gc_to != NULL);

    /* Copy stack data from old buffer to new buffer.
     * The stack lives at the high end of the memory block.
     * Source offset uses ORIGINAL mem_size (where stack lives in old_mem);
     * destination uses current mem_size (doubled, the new layout). */
    int src_stack_start = orig_mem_size + p->sp * (int)sizeof(Val);
    int dst_stack_start = p->mem_size + p->sp * (int)sizeof(Val);
    int stack_bytes = orig_mem_size - src_stack_start;
    if (stack_bytes > 0) {
        memcpy(p->mem + dst_stack_start, old_mem + src_stack_start, stack_bytes);
    }

    /* Clear new tospace (old fromspace) for next GC */
    memset(p->gc_to, 0, p->mem_size);
}

/* ============================================================
 * gc_fixup_heap_pointers — adjust all heap-internal Val pointers
 * after the mem buffer has been moved (e.g. by realloc in proc_grow).
 *
 * After realloc, the buffer may move to a different address.  All
 * pointer-type Vals (pairs, closures, strings, bytes) that referenced
 * objects inside the old buffer are stale.  This function walks the
 * compact heap and the stack, adding `delta` to every such pointer.
 * ============================================================ */

void gc_fixup_heap_pointers(Proc *p, intptr_t delta) {
    if (delta == 0)
        return;

    /* Old buffer range (before realloc moved it). delta = new - old,
     * so old_mem = p->mem - delta. Only heap pointers within this
     * range are stale; pointers to other heaps (e.g. scratch procs)
     * must NOT be adjusted. */
    uintptr_t old_lo = (uintptr_t)(p->mem - delta);
    uintptr_t old_hi = old_lo + p->heap_ptr;

    /* Walk compact heap [0, heap_ptr) and fix child Vals */
    fixup_buffer(p->mem, p->heap_ptr, delta, old_lo, old_hi);

    /* Fix stack Vals (stack lives at high end of mem) */
    Val *stack = (Val *)(p->mem + p->mem_size);
    for (int i = p->sp; i < 0; i++) {
        fixup_val_in_range(&stack[i], delta, old_lo, old_hi);
    }

    /* Fix gc_roots (may contain heap pointers from callers) */
    for (int i = 0; i < p->gc_root_count; i++) {
        fixup_val_in_range(&p->gc_roots[i], delta, old_lo, old_hi);
    }
}