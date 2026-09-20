/*
 * ta_inline.h — static inline helpers extracted from ta.h
 *
 * Split so ta.h can focus on types + declarations. Include via
 *   #include "ta_inline.h"
 * from ta.h (already done at the bottom).
 */
#ifndef TA_INLINE_H
#define TA_INLINE_H

#include <errno.h>
#include <stdio.h> /* ta_arena_fatal */

/* ============================================================
 * Value helpers
 * ============================================================ */

/* Extract the 16-bit tag from a NaN-boxed value. */
static inline uint16_t val_tag(Val v) { return (uint16_t)(v >> 48); }

/* Convenience: get HeapPair* from a TAG_PAIR Val */
static inline HeapPair *val_as_pair(Val v) {
    return (HeapPair *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

/* Get HeapClosure* from a TAG_CLOS Val */
static inline HeapClosure *val_as_clos(Val v) {
    return (HeapClosure *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

/* ============================================================
 * Actor heap arena — fixed reservation, promoted lazily
 *
 * An actor's heap + stack live in one `mem` buffer. The buffer is a
 * *fixed* reservation: it is never moved once it holds an object, and a
 * collection runs only while the GC gate is open (never with a live heap
 * reference held off the TA stack). Both together remove the need to root
 * Vals held in C locals — see docs/design-decisions.md D11.
 * ============================================================ */

/* Arena capacity in bytes (TA_ACTOR_HEAP), default 64 MiB, clamped to
 * [4 KiB, 1 GiB]. Values that are unset, non-numeric or out of range
 * leave the default in place rather than crashing. */
static inline int ta_arena_cap_env(void) {
    static int cached = -1; /* -1 = not parsed yet */
    if (cached < 0) {
        int cap = 64 * 1024 * 1024;
        const char *s = getenv("TA_ACTOR_HEAP");
        if (s && *s) {
            char *end = NULL;
            errno = 0;
            long v = strtol(s, &end, 10);
            if (errno == 0 && end != s && *end == '\0' && v >= 4096) {
                if (v > 1024L * 1024 * 1024)
                    v = 1024L * 1024 * 1024;
                cap = (int)v & ~7; /* heap offsets are 8-byte aligned */
            }
        }
        cached = cap;
    }
    return cached;
}

/* Running out of arena is fatal, and loud. The alternative — handing back
 * a nil Val and letting the program continue on a half-built structure —
 * turns a resource bound into a wrong answer. Same principle as the
 * procs[] bound in proc_new (issue #129). */
static inline void ta_arena_fatal(Proc *p, const char *what) {
    fprintf(stderr,
            "tavm: fatal: actor heap arena exhausted (%s) — pid %d: "
            "arena %d KiB, heap %d KiB, stack %d B, cap %d KiB. "
            "Raise TA_ACTOR_HEAP (bytes) to give each actor a bigger arena.\n",
            what, p->pid, p->mem_size / 1024, p->heap_ptr / 1024, -p->sp * (int)sizeof(Val),
            ta_arena_cap_env() / 1024);
    fflush(stderr);
    abort();
}

/* Reserve `need` bytes below the stack for this actor, doubling from the
 * 512-byte idling buffer up to the cap.
 *
 * Growth is only legal while the heap is empty: `Val` is an *absolute*
 * pointer into the buffer, so moving it would invalidate every pointer
 * to it — but with heap_ptr == 0 the arena contains no objects, hence no
 * Val anywhere can point into it (an actor's objects are only ever
 * referenced from its own stack/heap). Relocating is then invisible.
 * After the first object the buffer is frozen for the actor's lifetime.
 *
 * Returns 0 on success, -1 if it cannot grow that far (the caller
 * decides whether that is fatal). */
static inline int proc_arena_grow(Proc *p, int need) {
    if (p->mem == NULL || p->heap_ptr != 0)
        return -1;
    int cap = ta_arena_cap_env();
    int want = p->mem_size;
    while (want < need && want < cap)
        want *= 2;
    if (want > cap)
        want = cap;
    if (want <= p->mem_size)
        return -1; /* already at the cap */
    /* gc_to is interchangeable with mem after a swap, so it must never end
     * up *smaller* than mem_size — hence it is grown first. Leftovers in
     * the other direction (gc_to laps ahead of mem) are harmless. */
    if (p->gc_to != NULL) {
        uint8_t *new_gc = realloc(p->gc_to, want);
        if (new_gc == NULL)
            return -1;
        p->gc_to = new_gc;
    }
    uint8_t *new_mem = realloc(p->mem, want);
    if (new_mem == NULL)
        return -1;
    /* Stack data sits at the high end of the buffer; move it to the new
     * high end (the ranges overlap → memmove). */
    int old_off = p->mem_size + p->sp * (int)sizeof(Val);
    int new_off = want + p->sp * (int)sizeof(Val);
    int stack_bytes = p->mem_size - old_off;
    if (stack_bytes > 0)
        memmove(new_mem + new_off, new_mem + old_off, (size_t)stack_bytes);
    p->mem = new_mem;
    p->mem_size = want;
    return 0;
}

/* ============================================================
 * Lazy heap allocation
 * ============================================================ */

/* Lazily allocate the 512-byte idling buffer on first use. It only holds
 * the stack: the first heap object switches the actor to the full arena
 * reservation (proc_heap_alloc), because the buffer must never hold an
 * object that a later growth would invalidate. gc_to is NOT allocated
 * here — idle actors (blocked on recv) pay ~0 extra bytes. */
static inline void proc_ensure_heap(Proc *p) {
    if (p->mem == NULL) {
        p->mem_size = 512;
        p->mem = calloc(1, p->mem_size);
        /* gc_to stays NULL until first GC */
    }
}

/* ============================================================
 * Inline helpers — stack access
 * ============================================================ */

static inline Val *proc_stack(Proc *p) { return (Val *)(p->mem + p->mem_size); }

static inline void proc_push(Proc *p, Val v) {
    if (p->mem == NULL)
        proc_ensure_heap(p);
    /* The slot the stack is about to use (sp itself still points at the
     * old top); see the offset note below. */
    int off = p->mem_size + (p->sp - 1) * (int)sizeof(Val);
    if (off < p->heap_ptr) {
        /* Stack/heap collision. No collection happens here: proc_push never
         * allocates (proc_heap_alloc is the only collector), and the arena
         * cannot move once it holds an object. All that is left is to
         * reserve more room while the heap is still empty. */
        if (proc_arena_grow(p, (1 - p->sp) * (int)sizeof(Val)) != 0)
            ta_arena_fatal(p, "stack and heap meet before the new frame fits");
        off = p->mem_size + (p->sp - 1) * (int)sizeof(Val);
        if (off < p->heap_ptr)
            ta_arena_fatal(p, "stack and heap meet before the new frame fits");
    }
    p->sp--;
    /* NOTE: the slot address is computed as mem + (int offset) where the
     * offset stays within [0, mem_size) — the stack grows down from the
     * top of the buffer, so sp is negative but mem_size + sp*sizeof(Val)
     * is a non-negative in-buffer offset. Writing it as a single signed
     * index avoids the unsigned-wraparound pointer arithmetic UBSan
     * flags (sp * sizeof(Val) promotes to size_t, wrapping the GEP). */
    *(Val *)(p->mem + off) = v;
}

static inline Val proc_pop(Proc *p) {
    Val v = *(Val *)(p->mem + (p->mem_size + p->sp * (int)sizeof(Val)));
    p->sp++;
    return v;
}

static inline Val proc_peek(Proc *p, int offset) {
    return *(Val *)(p->mem + (p->mem_size + (p->sp + offset) * (int)sizeof(Val)));
}

/* ============================================================
 * Heap allocation helpers
 * ============================================================ */

/* Parse TA_GC_STRESS once (per translation unit copy; all copies agree).
 * Returns N (>= 1) — every N heap allocations request a collection — or 0
 * when the variable is unset or invalid (non-numeric, negative, overflow);
 * invalid values disable the knob rather than crashing. */
static inline int ta_gc_stress_env(void) {
    static int cached = -1; /* -1 = not parsed yet */
    if (cached < 0) {
        cached = 0;
        const char *s = getenv("TA_GC_STRESS");
        if (s && *s) {
            char *end = NULL;
            errno = 0;
            long v = strtol(s, &end, 10);
            if (errno == 0 && end != s && *end == '\0' && v >= 1) {
                if (v > 1000000L)
                    v = 1000000L; /* clamp absurd values */
                cached = (int)v;
            }
        }
    }
    return cached;
}

/* ============================================================
 * GC gate — in-place collection at allocation time
 *
 * A collection runs inside proc_heap_alloc (once the heap has outgrown
 * gc_trigger), not at an opcode boundary. That is only safe while no code
 * holds a live heap reference in a C local: the collector moves the live
 * set and forwards only the TA stack (its root set). A region that cannot
 * guarantee this — a C module callback, the iterative val_deep_copy
 * worklist, a nested val_pair chain, a dying proc's heap — closes the gate
 * around itself with proc_gc_enter()/proc_gc_leave(). The gate is a nesting
 * depth, so a region opened inside another balances naturally.
 *
 * While the gate is closed an allocation still records the request
 * (gc_pending); proc_gc_drain() honours it once the gate is open again.
 * Callers that leave a value unrooted on the way out (val_deep_copy's
 * result) skip the drain — the request stays pending and the next gate-open
 * allocation collects instead. */

static inline void proc_gc_enter(Proc *p) { p->gc_gate++; }

static inline void proc_gc_leave(Proc *p) { p->gc_gate--; }

/* Collect now if a request is pending and the gate is open; a no-op inside a
 * not-gc-safe region, so ordinary allocations call it unconditionally. An
 * unrooting region calls it once its live values are rooted again (or not at
 * all, leaving the request for the next allocation). */
static inline void proc_gc_drain(Proc *p) {
    if (p->gc_gate == 0 && p->gc_pending) {
        p->gc_pending = 0;
        gc_collect(p);
    }
}

/* Allocate `size` bytes on the actor's heap and zero them. Never returns
 * NULL: exhausting the arena is fatal (ta_arena_fatal), so callers have
 * no out-of-memory path to handle. */
#define TA_HEAP_MIN_OBJECT ((int)((sizeof(HeapHeader) + sizeof(void *) + 7) & ~7))

/* Forwarding pointers are written immediately after HeapHeader during GC.
 * Keep every heap allocation large enough for that temporary slot, including
 * zero-length strings/bytes. */
static inline int ta_heap_object_size(int size) {
    size = (size + 7) & ~7;
    return size < TA_HEAP_MIN_OBJECT ? TA_HEAP_MIN_OBJECT : size;
}

static inline void *proc_heap_alloc(Proc *p, int size) {
    /* GC stress knob (TA_GC_STRESS=N): ask for a collection on this proc
     * every N heap allocations. Off (single well-predicted branch) unless
     * the knob is enabled. Fresh procs have gc_stress_cnt == 0, which
     * counts down immediately, seeding the counter with N on first use. */
    if (ta_gc_stress_env() > 0 && --p->gc_stress_cnt <= 0) {
        p->gc_stress_cnt = ta_gc_stress_env();
        p->gc_pending = 1;
    }
    /* Align to 8 bytes and reserve space for GC's forwarding pointer. */
    size = ta_heap_object_size(size);

    if (p->mem == NULL)
        proc_ensure_heap(p);
    /* The idling buffer must never hold an object, because growth is not
     * possible once it does — so the first allocation switches the actor
     * to the full arena reservation. */
    if (p->heap_ptr == 0 && p->mem_size < ta_arena_cap_env()) {
        if (proc_arena_grow(p, ta_arena_cap_env()) != 0)
            ta_arena_fatal(p, "cannot reserve the actor arena");
    }
    /* Request a collection once the heap has outgrown the trigger, so the
     * live set stays well inside the arena, then collect in place if that is
     * safe right now (gate open). Must happen before the fit check: a
     * collection frees room the incoming object may need. */
    if (p->heap_ptr > p->gc_trigger)
        p->gc_pending = 1;
    proc_gc_drain(p);
    if (p->heap_ptr + size > p->mem_size + p->sp * (int)sizeof(Val))
        ta_arena_fatal(p, "allocation does not fit: heap + stack exceed the arena");
    void *ptr = p->mem + p->heap_ptr;
    p->heap_ptr += size;
    memset(ptr, 0, size);
    return ptr;
}

#endif /* TA_INLINE_H */