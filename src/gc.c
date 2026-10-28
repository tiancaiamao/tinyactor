/*
 * gc.c — Semispace copying garbage collector for TinyActor
 */

#include "ta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef GC_DEBUG
#include <assert.h>
#define GC_ASSERT(x) assert(x)
#else
#define GC_ASSERT(x) ((void)0)
#endif

/* gc_obj_size — 单个堆对象的完整分配大小（含 ta_heap_object_size 对齐），
 * resource.c 的线性空间扫描共用同一份尺寸表（步进必须与分配时逐字节
 * 一致，尺寸逻辑不能有两份）。 */
int gc_obj_size(void *obj) {
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
    case HEAP_RESOURCE:
        size = sizeof(HeapResource); /* 固定：header + owner_pid + res 指针 */
        break;
    default:
        GC_ASSERT(h->type == HEAP_PAIR || h->type == HEAP_CLOS || h->type == HEAP_STRING ||
                  h->type == HEAP_BYTES || h->type == HEAP_RESOURCE);
        fprintf(stderr, "gc: unknown heap type %d\n", h->type);
        abort();
    }
    return ta_heap_object_size(size);
}

static int in_fromspace(Proc *p, void *ptr) {
    return (uint8_t *)ptr >= p->mem && (uint8_t *)ptr < p->mem + p->heap_ptr;
}

/* Sources a collection copies from. Normally just fromspace; a convergence
 * collection (CallbackArena.promote) also copies reachable chunk objects —
 * they enter the same Cheney scan and forwarding discipline as fromspace
 * objects, which is what makes callback-exit promotion one mechanism
 * instead of a special one (issue #160). */
static int gc_copyable(Proc *p, void *ptr) {
    if (in_fromspace(p, ptr))
        return 1;
    return p->ccall != NULL && p->ccall->promote && val_in_chunk(p, ptr);
}

static int gc_collect_internal(Proc *p, int extra_room, Val *extra_root);

static void *gc_copy_obj(Proc *p, void *obj) {
    HeapHeader *h = (HeapHeader *)obj;
    GC_ASSERT(gc_copyable(p, obj));
    if (h->flags & FLAG_FORWARDED) {
        /* forwarding pointer stored after header */
        void *fwd;
        memcpy(&fwd, (uint8_t *)obj + sizeof(HeapHeader), sizeof(void *));
        return fwd;
    }
    int sz = gc_obj_size(obj);
    sz = (sz + 7) & ~7; /* align to 8 */
    /* The compacted live set has to land strictly below the destination's
     * stack area (the stack is copied in afterwards). gc_collect sized the
     * destination from an upper bound of the live set, so this is an
     * internal invariant — a violation means the bound was wrong, which is
     * a bug to fix, not a condition to handle. */
    if (p->gc_to_size + sz > p->gc_to_cap + p->sp * (int)sizeof(Val))
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
    if (tag != TAG_PAIR && tag != TAG_CLOS && tag != TAG_STRING && tag != TAG_BYTES &&
        tag != TAG_RESOURCE)
        return; /* immediate value, no heap pointer */
    void *ptr = (void *)(uintptr_t)(*v & 0x0000FFFFFFFFFFFFULL);
    if (!gc_copyable(p, ptr))
        return;
    void *new_ptr = gc_copy_obj(p, ptr);
    *v = (*v & 0xFFFF000000000000ULL) | (uint64_t)(uintptr_t)new_ptr;
}

static void gc_scan_tospace(Proc *p) {
    int scan = 0; /* the heap starts at the bottom of the block */
    while (scan < p->gc_to_size) {
        GC_ASSERT(scan <= p->gc_to_size);
        HeapHeader *h = (HeapHeader *)(p->gc_to + scan);
        GC_ASSERT(h->type == HEAP_PAIR || h->type == HEAP_CLOS || h->type == HEAP_STRING ||
                  h->type == HEAP_BYTES || h->type == HEAP_RESOURCE);
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
            /* HEAP_STRING / HEAP_BYTES / HEAP_RESOURCE have no child Val refs
             * (HEAP_RESOURCE 只拷贝 header：owner_pid/res 指针随 memcpy 一起
             * 进 tospace，payload 在堆外不受影响) */
        }
        int sz = gc_obj_size(h);
        scan += (sz + 7) & ~7;
    }
}

/* Collect the live set into a fresh semispace, growing the arena when the
 * caller needs room beyond what compaction alone frees.
 *
 * extra_room: usable bytes the caller needs on top of the current live
 * set (proc_heap_alloc passes the incoming object's size; a pure
 * compaction passes 0). Returns 0 on success, -1 when even the
 * TA_ACTOR_HEAP cap cannot fit live + extra — the caller decides whether
 * that is fatal.
 *
 * Growth needs no rebase pass anywhere else: the collection already
 * copies every live object and fixes every TA-stack Val, so "grow" and
 * "compact" are the same operation at different destination sizes. */
int gc_collect(Proc *p, int extra_room) { return gc_collect_internal(p, extra_room, NULL); }

/* Convergence slow path (issue #160): promote the callback's result graph
 * by collecting with the chunk arena as a second source space. *extra_root
 * (the result Val, still in the caller's local) is rooted and updated in
 * place. Falling short of the cap is a genuine out-of-memory — the result
 * has nowhere to land. */
void gc_collect_converge(Proc *p, Val *extra_root) {
    p->ccall->promote = 1;
    int rc = gc_collect_internal(p, p->ccall->total, extra_root);
    p->ccall->promote = 0;
    if (rc != 0)
        ta_arena_fatal(p, "callback result does not fit: heap + stack exceed the arena");
}

static int gc_collect_internal(Proc *p, int extra_room, Val *extra_root) {
    if (p->mem == NULL)
        return 0; /* idle proc with no heap — nothing to collect */
    GC_ASSERT(p->heap_ptr >= 0 && p->heap_ptr <= p->mem_size);

    /* Size the destination: an upper bound of the live set (heap_ptr),
     * the caller's extra room, and the stack — then doubling headroom so
     * growth is not immediately followed by another growth. */
    int cap = ta_arena_cap_env();
    int stack_bytes = -p->sp * (int)sizeof(Val);
    /* Sizing counts the deepest reserved frame floor (proc_stack_lo), not just
     * the live top: the heap-side fit invariant is heap_top <= floor, and
     * the collection is what restores it — sizing against the live top
     * alone would let the compacted heap land inside a frame whose pushes
     * come later (#248). stack_bytes stays sp-based: it doubles as the
     * memcpy length for the live stack region. */
    int stack_floor_bytes = -proc_stack_lo(p) * (int)sizeof(Val);
    int need = p->heap_ptr + extra_room + stack_floor_bytes;
    int want = p->mem_size;
    while (want < need && want < cap)
        want *= 2;
    if (want > cap)
        want = cap;
    if (want < need)
        return -1; /* does not fit even at the cap */

    /* Tospace is the previous fromspace, swapped after each collection;
     * grow the buffer when the arena outgrew it. */
    if (p->gc_to == NULL) {
        p->gc_to = calloc(1, want);
        if (p->gc_to == NULL)
            ta_arena_fatal(p, "cannot reserve the collection semispace");
        p->gc_to_cap = want;
    } else if (p->gc_to_cap < want) {
        uint8_t *grown = realloc(p->gc_to, want);
        if (grown == NULL)
            ta_arena_fatal(p, "cannot reserve the collection semispace");
        p->gc_to = grown;
        p->gc_to_cap = want;
    }
    p->gc_to_size = 0;

    /* Roots are exactly the TA stack (plus the convergence result, if any):
     * a collection runs from proc_heap_alloc only while the GC gate is
     * open, which callers guarantee means no code holds a live heap
     * reference outside this stack. */
    if (extra_root != NULL)
        gc_copy_val(p, extra_root);
    Val *stack = (Val *)(p->mem + p->mem_size);
    for (int i = p->sp; i < 0; i++) {
        gc_copy_val(p, &stack[i]);
    }

    /* Scan tospace (fix internal refs) */
    gc_scan_tospace(p);
    GC_ASSERT(p->gc_to_size <= p->gc_to_cap);

    /* Swap from/to. The old block is released instead of being kept as the
     * next tospace: a permanent second buffer would double every actor
     * that ever collected, while collections are rare — allocating the
     * next tospace inside that collection (the gc_to == NULL path above)
     * is the cheaper steady state. */
    uint8_t *old_mem = p->mem;
    int old_size = p->mem_size;
    /* fromspace 兜底扫描的上界：swap（下一行）会把 p->heap_ptr 覆盖成
     * tospace 的 live 大小，旧值必须先存下来（docs/c-resource-design.md §6.2）。 */
    int old_heap_ptr = p->heap_ptr;
    p->mem = p->gc_to;
    p->mem_size = p->gc_to_cap; /* == want */
    p->heap_ptr = p->gc_to_size;
    p->gc_to = NULL;
    p->gc_to_cap = 0;
    p->gc_to_size = 0;

    /* Copy stack data to the high end of the (possibly larger) new block. */
    int old_off = old_size + p->sp * (int)sizeof(Val);
    int new_off = p->mem_size + p->sp * (int)sizeof(Val);
    if (stack_bytes > 0) {
        memcpy(p->mem + new_off, old_mem + old_off, (size_t)stack_bytes);
    }

    /* NOTE: the old fromspace is deliberately not cleared. Nothing reads
     * past the live region: gc_scan_tospace walks [0, gc_to_size) only, and
     * every slot the allocator hands out is zeroed by proc_heap_alloc, so
     * stale headers — including their FLAG_FORWARDED bits — are never
     * observed. memset'ing the buffer would commit every page of the arena
     * on each cycle for nothing. */
    /* fromspace 兜底释放（§6.2）：旧空间里未被转发的 HEAP_RESOURCE 是
     * 确认不可达的垃圾，owner_pid == 本进程的跑 dtor。被转发的副本已进
     * tospace，跳过——forwarding 指针踩踏过它的字段（含 res 低 2 字节），
     * 见 resource.c resource_release_space 注释。 */
    resource_release_space(old_mem, old_heap_ptr, p->pid, 1);
    free(old_mem);

    /* When to collect next: once the heap has grown to twice the live set —
     * garbage stays under 50% of the arena. The floor keeps tiny actors
     * from collecting on every allocation. No ceiling: trigger is always
     * above the post-collection heap_ptr, so a stable live set (e.g. a
     * compiler's AST) never degrades into a collection per allocation;
     * room is found by the growth collections instead. */
    p->gc_trigger = 2 * p->heap_ptr;
    if (p->gc_trigger < 4096)
        p->gc_trigger = 4096;
    return 0;
}
