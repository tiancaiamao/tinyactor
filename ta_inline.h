/*
 * ta_inline.h — static inline helpers extracted from ta.h
 *
 * Split so ta.h can focus on types + declarations. Include via
 *   #include "ta_inline.h"
 * from ta.h (already done at the bottom).
 */
#ifndef TA_INLINE_H
#define TA_INLINE_H

#include <assert.h>
#include <errno.h>
#include <stdio.h> /* ta_arena_fatal */

/* ============================================================
 * Value helpers
 * ============================================================ */

/* Extract the 16-bit tag from a NaN-boxed value. */
static inline uint16_t val_tag(Val v) { return (uint16_t)(v >> 48); }

/* Value predicates on hot paths (arithmetic/comparison gates): static inline
 * so a tag test compiles to a register test, never an out-of-line call. */

static inline int val_is_int(Val v) { return val_tag(v) == TAG_INT; }

/* Float discrimination: top byte != 0xFF. All NaN-boxed tags have top byte
 * 0xFF, so this is exactly "not a tagged value". -NaN/-Inf collide (top byte
 * 0xFF) — the baseline never constructs NaN and only produces +Inf from
 * division by zero. */
static inline int val_is_float(Val v) { return ((v >> 56) & 0xFF) != 0xFF; }

/* Convenience: get HeapPair* from a TAG_PAIR Val */
static inline HeapPair *val_as_pair(Val v) {
    return (HeapPair *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

/* Get HeapClosure* from a TAG_CLOS Val */
static inline HeapClosure *val_as_clos(Val v) {
    return (HeapClosure *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

/* ============================================================
 * Actor heap arena — small initial block, grows at collection points
 *
 * An actor's heap + stack live in one `mem` block that starts tiny (the
 * 512-byte idling buffer, then TA_PROC_HEAP0 once the first heap object
 * appears) and grows by doubling, up to the TA_ACTOR_HEAP cap. Growth
 * always goes through a collection: the live set is copied into a fresh,
 * larger semispace (gc_collect), which is what keeps every `Val` on the
 * TA stack valid across the move. A collection runs only while the GC
 * gate is open (never with a live heap reference held off the TA stack).
 * Together these remove the need to root Vals held in C locals — see
 * docs/design-decisions.md D11.
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

/* Reserve `need` usable bytes (excluding the chunk slice) below the stack
 * for this actor, doubling from the 512-byte idling buffer up to the cap.
 *
 * Growth is only legal while the heap is empty: `Val` is an *absolute*
 * pointer into the buffer, so moving it would invalidate every pointer
 * to it — but with the heap empty the arena contains no objects, hence no
 * Val anywhere can point into it (an actor's objects are only ever
 * referenced from its own stack/heap). Relocating is then invisible.
 * Once objects exist, growth goes through a collection instead
 * (gc_collect) — see proc_heap_alloc.
 *
 * Returns 0 on success, -1 if it cannot grow that far (the caller
 * decides whether that is fatal). */
static inline int proc_arena_grow(Proc *p, int need) {
    if (p->mem == NULL || p->heap_ptr > TA_PROC_CHUNK0)
        return -1;
    int cap = ta_arena_cap_env();
    int want = p->mem_size;
    while (want - TA_PROC_CHUNK0 < need && want < cap)
        want *= 2;
    if (want > cap)
        want = cap;
    if (want - TA_PROC_CHUNK0 < need)
        return -1; /* does not fit even at the cap */
    if (want <= p->mem_size)
        return -1; /* already at the cap */
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
 * the stack: the first heap object switches the actor to the TA_PROC_HEAP0
 * initial heap (proc_heap_alloc), because the buffer must never hold an
 * object that a relocation would invalidate — growth without a collection
 * is only legal while the heap is empty. gc_to is NOT allocated here —
 * idle actors (blocked on recv) pay ~0 extra bytes. */
static inline int proc_heap_empty(Proc *p) { return p->heap_ptr <= TA_PROC_CHUNK0; }

static inline void proc_ensure_heap(Proc *p) {
    if (p->mem == NULL) {
        p->mem_size = 512 + TA_PROC_CHUNK0;
        p->heap_ptr = TA_PROC_CHUNK0; /* no objects yet; heap starts above the chunk slice */
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

/* Make sure stack slots down to `lo_idx` (a negative proc_stack index) fit
 * above the heap, growing the arena if needed. Same collision discipline as
 * proc_push — growth is only legal while the heap is still empty (an arena
 * that already holds an object can never move) — but reserves a whole range
 * at once, for handlers that rearrange the stack with memmove rather than
 * one slot at a time (OP_CALL / OP_TAIL_CALL). */
static inline void proc_stack_reserve(Proc *p, int lo_idx) {
    if (p->mem == NULL)
        proc_ensure_heap(p);
    if (p->mem_size + lo_idx * (int)sizeof(Val) < p->heap_ptr) {
        if (proc_arena_grow(p, (1 - lo_idx) * (int)sizeof(Val)) != 0)
            ta_arena_fatal(p, "stack and heap meet before the new frame fits");
        if (p->mem_size + lo_idx * (int)sizeof(Val) < p->heap_ptr)
            ta_arena_fatal(p, "stack and heap meet before the new frame fits");
    }
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

static inline void proc_gc_leave(Proc *p) {
    /* Balancing every exit is the whole discipline, so a region that leaves
     * more often than it enters is a bug: catch it here rather than let the
     * depth go negative, where a later region's single enter reads back as 0
     * and the collector runs in the middle of it — the very UAF this gate
     * exists to prevent. Together with the per-instruction check in
     * vm_run_proc, this turns a missed exit into a deterministic crash under
     * TA_GC_STRESS=1. assert is live unless NDEBUG, so a release build
     * (-DNDEBUG) pays nothing. */
    assert(p->gc_gate > 0);
    p->gc_gate--;
}

/* Collect now if a request is pending and the gate is open; a no-op inside a
 * not-gc-safe region, so ordinary allocations call it unconditionally. An
 * unrooting region calls it once its live values are rooted again (or not at
 * all, leaving the request for the next allocation). */
static inline void proc_gc_drain(Proc *p) {
    if (p->gc_gate == 0 && p->gc_pending) {
        p->gc_pending = 0;
        gc_collect(p, 0);
    }
}

/* Reopen the gate for a region whose live values are rooted again, and honour
 * the request the closed region had to suppress — the ordinary exit of a gate
 * region. The counterpart is a bare proc_gc_leave(): it leaves a value
 * unrooted (val_deep_copy's result, a dying proc's DOWN walk), so the pending
 * request waits for the next gate-open allocation instead. */
static inline void proc_gc_reopen(Proc *p) {
    proc_gc_leave(p);
    proc_gc_drain(p);
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

/* ============================================================
 * Chunk arena — the C callback's private heap (gate closed)
 *
 * While the GC gate is closed every proc_heap_alloc lands here: a bump
 * allocator over a chain of chunks whose head is the TA_PROC_CHUNK0 slice
 * at the bottom of the `mem` block. Two properties make this the whole
 * safety story for C modules (issue #160):
 *
 *   1. Arena frozen — no heap allocation happens during a callback, so no
 *      collection or relocation can invalidate the Vals/pointers the
 *      callback holds. Zero rules for module authors.
 *   2. Chunk objects never move — the chain only grows (a full chunk is
 *      retired, never realloc'd), so pointers into chunks stay valid even
 *      across arena growth or collection on the TA side.
 *
 * Chunk objects are ordinary tagged heap objects, but gc_copy_val skips
 * pointers outside fromspace, so they are invisible to collections. At
 * OP_CCALL exit the result converges into the heap (proc_chunk_converge
 * in gc.c) and the chain resets: the inline chunk is kept, malloc'd
 * chunks are freed.
 * ============================================================ */

/* Malloc'd capacity for overflow chunks. Tunable: callbacks whose results
 * outgrow TA_PROC_CHUNK0 pay one malloc per TA_PROC_CHUNK_NEXT bytes. */
#ifndef TA_PROC_CHUNK_NEXT
#define TA_PROC_CHUNK_NEXT 4096
#endif

/* Free bytes kept between heap top and stack bottom. Opcode handlers hold
 * Vals — and raw heap pointers — in C locals across proc_push /
 * proc_stack_reserve, so the arena must not move inside a handler; the
 * collision paths there are fatal. This headroom is maintained at the VM's
 * instruction boundary (the one safe point where the TA stack is the whole
 * root set), which keeps those collision paths unreachable for well-formed
 * bytecode. Tunable: it bounds the stack an opcode may grow in one step
 * (frames are a few slots; TA_STACK_HEADROOM / sizeof(Val) slots is ample). */
#ifndef TA_STACK_HEADROOM
#define TA_STACK_HEADROOM (8 * 1024)
#endif

/* Enforce TA_STACK_HEADROOM at an instruction boundary: grow the arena by
 * collection. Only meaningful once the heap holds objects — with an empty
 * heap the push/reserve collision paths can still grow the arena by plain
 * reservation (nothing to invalidate), so fresh/idling actors are not
 * forced to TA_STACK_HEADROOM-sized blocks just for running a few opcodes.
 * The caller must have the GC gate open. */
static inline void proc_stack_headroom(Proc *p) {
    if (p->mem == NULL || proc_heap_empty(p))
        return;
    if (p->mem_size - p->heap_ptr + p->sp * (int)sizeof(Val) >= TA_STACK_HEADROOM)
        return;
    if (gc_collect(p, TA_STACK_HEADROOM) != 0)
        ta_arena_fatal(p, "stack outgrew the arena");
}

/* Guarantee room for `bytes` of heap objects (plus stack headroom) before
 * a gate-closed rebuild whose size is known up front — deep-copying an
 * incoming message, a spawned closure, a callback result. The copy itself
 * cannot grow the arena (its worklist holds unrooted Vals), so the room
 * must exist beforehand. The gate must be open here: growth is a
 * collection rooted at the TA stack (or a plain reservation while the
 * heap is empty). */
static inline void proc_reserve_heap(Proc *p, int bytes) {
    if (p->mem_size - p->heap_ptr + p->sp * (int)sizeof(Val) >= bytes)
        return;
    if (proc_heap_empty(p)) {
        if (proc_arena_grow(p, p->heap_ptr - TA_PROC_CHUNK0 + bytes + TA_STACK_HEADROOM) != 0)
            ta_arena_fatal(p, "cannot reserve arena room for an incoming copy");
    } else if (gc_collect(p, bytes + TA_STACK_HEADROOM) != 0) {
        ta_arena_fatal(p, "incoming data does not fit: heap + stack exceed the arena");
    }
}

static inline void *proc_chunk_alloc(Proc *p, int size) {
    TaChunk *c = p->gc_ck_cur;
    if (c->used + size > c->cap) {
        /* Retire the current chunk and open a fresh one; oversized objects
         * get a chunk of their own. Chunks are never realloc'd or moved. */
        int cap = size > TA_PROC_CHUNK_NEXT ? size : TA_PROC_CHUNK_NEXT;
        TaChunk *nc = malloc(sizeof(TaChunk) + (size_t)cap);
        if (nc == NULL)
            ta_arena_fatal(p, "out of memory allocating a callback chunk");
        nc->next = NULL;
        nc->used = 0;
        nc->cap = cap;
        /* The current chunk is always the last in the chain. */
        if (c == &p->gc_ck_head)
            p->gc_ck_head.next = nc;
        else
            c->next = nc;
        p->gc_ck_cur = c = nc;
    }
    uint8_t *data = (c == &p->gc_ck_head) ? p->mem : (uint8_t *)(c + 1);
    void *ptr = data + c->used;
    c->used += size;
    p->gc_ck_total += size;
    memset(ptr, 0, size);
    return ptr;
}

/* Free the whole callback arena. Only called when nothing can reference
 * chunk objects any more (after convergence at OP_CCALL exit, or when the
 * proc dies) — chunk objects are not individually tracked, the arena dies
 * as a whole. */
static inline void proc_chunk_reset(Proc *p) {
    TaChunk *c = p->gc_ck_head.next;
    while (c != NULL) {
        TaChunk *next = c->next;
        free(c);
        c = next;
    }
    p->gc_ck_head.next = NULL;
    p->gc_ck_head.used = 0;
    p->gc_ck_cur = &p->gc_ck_head;
    p->gc_ck_total = 0;
}

/* Is this pointer into the callback chunk arena? The inline chunk occupies
 * [mem, mem + TA_PROC_CHUNK0); malloc'd chunks live on their own. */
static inline int val_in_chunk(Proc *p, void *ptr) {
    uint8_t *q = (uint8_t *)ptr;
    if (q >= p->mem && q < p->mem + TA_PROC_CHUNK0)
        return 1;
    for (TaChunk *c = p->gc_ck_head.next; c != NULL; c = c->next)
        if (q >= (uint8_t *)(c + 1) && q < (uint8_t *)(c + 1) + c->used)
            return 1;
    return 0;
}

/* OP_CCALL exit: copy the callback's result from the chunk arena into the
 * heap, then free the arena. The gate may be closed here (vm.c converges
 * before reopening it); chunk addresses are stable regardless.
 *
 * Fast path — the heap has room for the whole arena (its total is an upper
 * bound for the result, objects copy at identical size): a plain deep copy
 * that passes arena references through and rebuilds only chunk objects.
 * Gate bump keeps collections out (the worklist holds unrooted Vals) while
 * gc_ck_converge routes the copy's own allocations to the heap.
 *
 * Slow path — one collection with the chunk arena as a second source space:
 * reachable chunk objects promote through the normal Cheney scan, and
 * forwarding pointers rewrite every reference (gc_collect_converge).
 *
 * The pending-request is left undrained on the way out: the result is not
 * rooted until the caller pushes it. */
static inline void proc_chunk_converge(Proc *p, Val *result) {
    if (p->gc_ck_total == 0)
        return;
    int free_heap = p->mem_size + p->sp * (int)sizeof(Val) - p->heap_ptr;
    if (free_heap >= p->gc_ck_total) {
        proc_gc_enter(p);
        p->gc_ck_converge = 1;
        *result = val_converge_copy(p, *result);
        p->gc_ck_converge = 0;
        proc_chunk_reset(p);
        proc_gc_leave(p);
    } else {
        gc_collect_converge(p, result);
        proc_chunk_reset(p);
    }
}

static inline void *proc_heap_alloc(Proc *p, int size) {
    /* Align to 8 bytes and reserve space for GC's forwarding pointer. */
    size = ta_heap_object_size(size);

    /* Gate closed (C callback): allocate from the chunk arena. This keeps
     * the arena itself frozen for the whole callback — no collection can
     * run, so every Val and raw pointer the callback holds stays valid —
     * and the result converges into the heap at OP_CCALL exit. Routed by
     * the callback window flag, not the gate: the gate also closes inside
     * the VM (message delivery, DOWN walks), whose allocations belong in
     * the heap. */
    if (p->in_ccall && !p->gc_ck_converge)
        return proc_chunk_alloc(p, size);

    /* GC stress knob (TA_GC_STRESS=N): ask for a collection on this proc
     * every N heap allocations. Off (single well-predicted branch) unless
     * the knob is enabled. Fresh procs have gc_stress_cnt == 0, which
     * counts down immediately, seeding the counter with N on first use. */
    if (ta_gc_stress_env() > 0 && --p->gc_stress_cnt <= 0) {
        p->gc_stress_cnt = ta_gc_stress_env();
        p->gc_pending = 1;
    }

    if (p->mem == NULL)
        proc_ensure_heap(p);
    /* The idling buffer must never hold an object, because growth without a
     * collection is not possible once it does — so the first allocation
     * switches the actor to the TA_PROC_HEAP0 initial heap. Further growth
     * happens through collections (below), not reservations. */
    if (proc_heap_empty(p) && p->mem_size - TA_PROC_CHUNK0 < TA_PROC_HEAP0) {
        if (proc_arena_grow(p, TA_PROC_HEAP0) != 0)
            ta_arena_fatal(p, "cannot reserve the actor arena");
    }
    /* Request a collection once the heap has outgrown the trigger, so the
     * live set stays well inside the arena, then collect in place if that is
     * safe right now (gate open). Must happen before the fit check: a
     * collection frees room the incoming object may need. */
    if (p->heap_ptr > p->gc_trigger)
        p->gc_pending = 1;
    proc_gc_drain(p);
    if (p->heap_ptr + size > p->mem_size + p->sp * (int)sizeof(Val)) {
        /* Out of room: compact the live set and grow the arena in one
         * collection (the live set + `size` land in a larger semispace).
         * TA_STACK_HEADROOM keeps the stack clear of the new heap top so
         * handlers never collide (see proc_stack_headroom). */
        if (p->gc_gate != 0 || gc_collect(p, size + TA_STACK_HEADROOM) != 0)
            ta_arena_fatal(p, "allocation does not fit: heap + stack exceed the arena");
    }
    void *ptr = p->mem + p->heap_ptr;
    p->heap_ptr += size;
    memset(ptr, 0, size);
    return ptr;
}

#endif /* TA_INLINE_H */