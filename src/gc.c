/*
 * gc.c — Semispace copying garbage collector for TinyActor
 */

#include "ta.h"
#include <string.h>
#include <stdlib.h>

#define FLAG_FORWARDED 0x01

static int obj_size(void *obj) {
    HeapHeader *h = (HeapHeader *)obj;
    switch (h->type) {
        case HEAP_PAIR:   return sizeof(HeapPair);
                case HEAP_CLOS: return sizeof(HeapClosure) + ((HeapClosure*)obj)->nfree * (int)sizeof(Val);
        case HEAP_STRING:  return sizeof(HeapString) + ((HeapString*)obj)->len + 1;
        case HEAP_BYTES:   return sizeof(HeapBytes) + ((HeapBytes*)obj)->len;
        default: return sizeof(HeapHeader); /* unknown, copy minimum */
    }
}

static int in_fromspace(Proc *p, void *ptr) {
    return (uint8_t*)ptr >= p->mem && (uint8_t*)ptr < p->mem + p->heap_ptr;
}

static void *gc_copy_obj(Proc *p, void *obj) {
    HeapHeader *h = (HeapHeader *)obj;
    if (h->flags & FLAG_FORWARDED) {
        /* forwarding pointer stored after header */
        void **fp = (void **)((uint8_t*)obj + sizeof(HeapHeader));
        return *fp;
    }
    int sz = obj_size(obj);
    sz = (sz + 7) & ~7; /* align to 8 */
    void *new_obj = p->gc_to + p->gc_to_size;
    memcpy(new_obj, obj, sz);
    p->gc_to_size += sz;
    /* leave forwarding pointer */
    h->flags |= FLAG_FORWARDED;
    void **fp = (void **)((uint8_t*)obj + sizeof(HeapHeader));
    *fp = new_obj;
    return new_obj;
}

static void gc_copy_val(Proc *p, Val *v) {
    uint64_t tag = val_tag(*v);
    if (tag != TAG_PAIR && tag != TAG_CLOS && tag != TAG_STRING && tag != TAG_BYTES)
        return; /* immediate value, no heap pointer */
    void *ptr = (void *)(uintptr_t)(*v & 0x0000FFFFFFFFFFFFULL);
    if (!in_fromspace(p, ptr)) return;
    void *new_ptr = gc_copy_obj(p, ptr);
    *v = (*v & 0xFFFF000000000000ULL) | (uint64_t)(uintptr_t)new_ptr;
}

static void gc_scan_tospace(Proc *p) {
    int scan = 0;
    while (scan < p->gc_to_size) {
        HeapHeader *h = (HeapHeader *)(p->gc_to + scan);
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
    p->gc_to_size = 0;

    /* Scan stack roots */
    Val *stack = (Val *)(p->mem + p->mem_size);
    for (int i = p->sp; i < 0; i++) {
        gc_copy_val(p, &stack[i]);
    }

    /* Scan mailbox */
    if (p->mbox && p->mbox_count > 0) {
        for (int i = 0; i < p->mbox_count; i++) {
            int idx = (p->mbox_head + i) % p->mbox_cap;
            gc_copy_val(p, &p->mbox[idx]);
        }
    }

    /* Scan gc_roots */
    for (int i = 0; i < p->gc_root_count; i++) {
        gc_copy_val(p, &p->gc_roots[i]);
    }

    /* Scan tospace (fix internal refs) */
    gc_scan_tospace(p);

        /* Swap from/to */
    uint8_t *old_mem = p->mem;
    int old_mem_size = p->mem_size;
    p->mem = p->gc_to;
    p->heap_ptr = p->gc_to_size;
    p->mem_size = old_mem_size;
    p->gc_to = old_mem;
    p->gc_to_size = 0;

    /* Copy stack data from old buffer to new buffer.
     * The stack lives at the high end of the memory block. */
    int stack_start = p->mem_size + p->sp * (int)sizeof(Val);
    int stack_bytes = p->mem_size - stack_start;
    if (stack_bytes > 0) {
        memcpy(p->mem + stack_start, old_mem + stack_start, stack_bytes);
    }

    /* Clear new tospace (old fromspace) for next GC */
    memset(p->gc_to, 0, p->mem_size);
}