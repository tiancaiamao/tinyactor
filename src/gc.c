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
    int size;
    switch (h->type) {
    case HEAP_PAIR:
        size = sizeof(HeapPair);
        break;
    case HEAP_CLOS:
        size = sizeof(HeapClosure) + ((HeapClosure *)obj)->nfree * (int)sizeof(Val);
        break;
    case HEAP_STRING:
        size = sizeof(HeapString) + ((HeapString *)obj)->len + 1;
        break;
    case HEAP_BYTES:
        size = sizeof(HeapBytes) + ((HeapBytes *)obj)->len;
        break;
    default:
        GC_ASSERT(h->type == HEAP_PAIR || h->type == HEAP_CLOS || h->type == HEAP_STRING ||
                  h->type == HEAP_BYTES);
        fprintf(stderr, "gc: unknown heap type %d\n", h->type);
        abort();
    }
    return ta_heap_object_size(size);
}

static int in_fromspace(Proc *p, void *ptr) {
    return (uint8_t *)ptr >= p->mem && (uint8_t *)ptr < p->mem + p->heap_ptr;
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
    /* The compacted live set has to land strictly below the destination's
     * stack area (the stack is copied in afterwards). Checked *before*
     * writing: with a fixed arena there is no growth to fall back on, and
     * running past the buffer would silently corrupt the actor's heap. */
    if (p->gc_to_size + sz > p->mem_size + p->sp * (int)sizeof(Val))
        ta_arena_fatal(p, "live set does not fit below the stack during collection");
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
    /* Ensure gc_to is allocated for this GC cycle. It is lazily
     * allocated to match mem_size. After the swap below, gc_to will
     * point to the old fromspace and remain available for next GC.
     * Idle actors that never trigger GC never pay this cost. */
    if (p->gc_to == NULL) {
        p->gc_to = calloc(1, p->mem_size);
        if (p->gc_to == NULL)
            ta_arena_fatal(p, "cannot reserve the collection semispace");
    }
    p->gc_to_size = 0;

    /* Roots are exactly the TA stack: a collection never runs inside an
     * opcode handler (vm_step owns the boundary) and the arena never
     * moves, so nothing else can be holding a pointer that needs to be
     * seen or fixed up. */
    Val *stack = (Val *)(p->mem + p->mem_size);
    for (int i = p->sp; i < 0; i++) {
        gc_copy_val(p, &stack[i]);
    }

    /* Scan tospace (fix internal refs) */
    gc_scan_tospace(p);

    /* Pre-swap assertion: live data must fit in heap */
    GC_ASSERT(p->gc_to_size >= 0);
    GC_ASSERT(p->gc_to_size <= p->mem_size);

    /* Swap from/to */
    uint8_t *old_mem = p->mem;
    p->mem = p->gc_to;
    p->heap_ptr = p->gc_to_size;
    p->gc_to = old_mem;
    p->gc_to_size = 0;

    /* Post-swap assertions */
    GC_ASSERT(p->gc_to != NULL);

    /* Copy stack data from old buffer to new buffer.
     * The stack lives at the high end of the memory block, and a
     * collection does not change mem_size, so source and destination are
     * at the same offset. */
    int stack_start = p->mem_size + p->sp * (int)sizeof(Val);
    int stack_bytes = p->mem_size - stack_start;
    if (stack_bytes > 0) {
        memcpy(p->mem + stack_start, old_mem + stack_start, (size_t)stack_bytes);
    }

    /* NOTE: the old fromspace is deliberately not cleared. Nothing reads
     * past the live region: gc_scan_tospace walks [0, gc_to_size) only,
     * and every slot the allocator hands out is zeroed by
     * proc_heap_alloc, so stale headers — including their FLAG_FORWARDED
     * bits — are never observed. memset'ing the buffer would commit every
     * page of the arena on each cycle for nothing. */

    /* When to collect next: once the heap has grown to twice the live set.
     * The floor keeps tiny actors from collecting on every allocation; the
     * ceiling keeps a stack/heap collision meaning "the live set really
     * does not fit" instead of "the collector is behind". */
    p->gc_trigger = 2 * p->heap_ptr;
    if (p->gc_trigger < 4096)
        p->gc_trigger = 4096;
    if (p->gc_trigger > p->mem_size * 3 / 4)
        p->gc_trigger = p->mem_size * 3 / 4;
}
