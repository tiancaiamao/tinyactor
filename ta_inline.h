/*
 * ta_inline.h — static inline helpers extracted from ta.h
 *
 * Split so ta.h can focus on types + declarations. Include via
 *   #include "ta_inline.h"
 * from ta.h (already done at the bottom).
 */
#ifndef TA_INLINE_H
#define TA_INLINE_H

/* ============================================================
 * Value helpers
 * ============================================================ */

/* Extract the 16-bit tag from a NaN-boxed value. */
static inline uint16_t val_tag(Val v) {
    return (uint16_t)(v >> 48);
}

/* Convenience: get HeapPair* from a TAG_PAIR Val */
static inline HeapPair *val_as_pair(Val v) {
    return (HeapPair *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

/* Get HeapClosure* from a TAG_CLOS Val */
static inline HeapClosure *val_as_clos(Val v) {
    return (HeapClosure *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

/* ============================================================
 * GC root guards — push/pop to protect C-local Vals across GC
 * ============================================================ */

static inline void gc_root_push(Proc *p, Val v) {
    if (p->gc_roots == NULL) {
        p->gc_roots_cap = 32;
        p->gc_roots = malloc(p->gc_roots_cap * sizeof(Val));
    }
    DA_GROW(p->gc_roots, p->gc_root_count, p->gc_roots_cap);
    p->gc_roots[p->gc_root_count++] = v;
}
static inline Val gc_root_pop(Proc *p) {
    return p->gc_roots[--p->gc_root_count];
}

/* ============================================================
 * Lazy heap allocation
 * ============================================================ */

/* Lazily allocate mem on first use. gc_to is NOT allocated here —
 * it is only allocated on-demand by gc_collect when GC actually
 * runs. This means idle actors (blocked on recv) use ~0 extra bytes
 * beyond the initial heap. */
static inline void proc_ensure_heap(Proc *p) {
    if (p->mem == NULL) {
        p->mem_size = 1024;
        p->mem      = calloc(1, p->mem_size);
        /* gc_to stays NULL until first GC */
    }
}

/* ============================================================
 * Inline helpers — stack access
 * ============================================================ */

static inline Val *proc_stack(Proc *p) {
    return (Val *)(p->mem + p->mem_size);
}

/* forward declaration — needed by proc_push */
static inline int proc_grow(Proc *p);

static inline void proc_push(Proc *p, Val v) {
    if (p->mem == NULL) proc_ensure_heap(p);
    p->sp--;
    /* Check for stack-heap collision before writing.
     * The stack grows downward and the heap grows upward;
     * if they meet, trigger GC to reclaim space. */
    if (p->mem_size + p->sp * (int)sizeof(Val) <= p->heap_ptr) {
        /* Collision! Protect v during GC and possible grow. */
        gc_root_push(p, v);
        gc_collect(p);
        if (p->mem_size + p->sp * (int)sizeof(Val) <= p->heap_ptr) {
            /* GC didn't free enough — grow memory.
             * v is protected in gc_roots; proc_grow calls
             * gc_fixup_heap_pointers which fixes gc_roots too. */
            proc_grow(p);
        }
        v = gc_root_pop(p);
    }
    *(Val *)(p->mem + p->mem_size + p->sp * sizeof(Val)) = v;
}

static inline Val proc_pop(Proc *p) {
    Val v = *(Val *)(p->mem + p->mem_size + p->sp * sizeof(Val));
    p->sp++;
    return v;
}

static inline Val proc_peek(Proc *p, int offset) {
    return *(Val *)(p->mem + p->mem_size + (p->sp + offset) * sizeof(Val));
}

/* ============================================================
 * Heap allocation helpers
 * ============================================================ */

/* Allocate `size` bytes on the process heap. Returns NULL if OOM. */
static inline void *proc_heap_alloc(Proc *p, int size) {
    /* Align to 8 bytes */
    size = (size + 7) & ~7;
    if (p->mem == NULL) proc_ensure_heap(p);
    if (p->heap_ptr + size > p->mem_size + p->sp * (int)sizeof(Val)) {
        /* heap-stack collision — trigger GC and retry */
        gc_collect(p);
        /* Keep growing until allocation fits or growth fails.
         * Initial heap (1024) may need multiple doublings for
         * large string allocations (e.g. file.read on >1KB files). */
        while (p->heap_ptr + size > p->mem_size + p->sp * (int)sizeof(Val)) {
            if (proc_grow(p) != 0) return NULL;
        }
    }
    void *ptr = p->mem + p->heap_ptr;
    p->heap_ptr += size;
    memset(ptr, 0, size);
    return ptr;
}

static inline int proc_grow(Proc *p) {
    int new_size = p->mem_size ? p->mem_size * 2 : 1024;
    /* Only grow gc_to if it exists (may be NULL if GC never ran) */
    if (p->gc_to) {
        uint8_t *new_gc = realloc(p->gc_to, new_size);
        if (!new_gc) return -1;
        p->gc_to = new_gc;
    }
    uint8_t *new_mem = realloc(p->mem, new_size);
    if (!new_mem) return -1;
    /* Relocate stack data to the new high end of the memory block.
     * The stack grows downward from mem+mem_size; after doubling
     * mem_size, the stack base moves but the data hasn't. */
    {
        int old_stack_off = p->mem_size + p->sp * (int)sizeof(Val);
        int new_stack_off = new_size + p->sp * (int)sizeof(Val);
        int stack_bytes = p->mem_size - old_stack_off;
        if (stack_bytes > 0)
            memcpy(new_mem + new_stack_off, new_mem + old_stack_off, stack_bytes);
    }
    /* If realloc moved the buffer, fix all heap-internal absolute pointers */
    intptr_t delta = (intptr_t)(new_mem - p->mem);
    p->mem = new_mem;
    /* gc_to already updated above if it existed; stays NULL otherwise */
    p->mem_size = new_size;
    if (delta != 0)
        gc_fixup_heap_pointers(p, delta);
    if (p->gc_to)
        memset(p->gc_to, 0, new_size);
    return 0;
}

#endif /* TA_INLINE_H */