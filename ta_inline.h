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

/* Truthiness for the branch opcodes (LOP_JIF). Two tag
 * tests — never an out-of-line call: as a cross-TU call every conditional
 * branch in a loop paid a bl/ret. */
static inline int val_is_true(Val v) { return val_tag(v) != TAG_NIL && val_tag(v) != TAG_FALSE; }

/* Nil/true/false constants. As out-of-line calls in val.c these were a bl/ret
 * each and — worse — clang could not constant-fold them, so every "return
 * nil" on a stage exit (RT_YIELD, proc_die, an empty-list tail) stayed a real
 * call in the C backend's generated code (profiled at ~3% of a compile). */
static inline Val val_nil(void) { return lbox(TAG_NIL, 0); }
static inline Val val_true(void) { return lbox(TAG_TRUE, 0); }
static inline Val val_false(void) { return lbox(TAG_FALSE, 0); }

static inline int val_is_pair(Val v) { return val_tag(v) == TAG_PAIR; }

/* Convenience: get HeapString* from a TAG_STRING Val */
static inline HeapString *val_as_string(Val v) {
    return (HeapString *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

/* Equality used by OP_EQ/OP_NE (and the C backend's rt_eq): strings compare
 * by content, everything else by the NaN-boxed value — ints/symbols/nil/true/
 * false by value, heap objects (pair/closure/bytes/pid) by pointer identity.
 * The `a == b` test first is the common case (identical immediates, or a heap
 * object compared with itself); two equal-but-distinct strings fall through
 * to the memcmp. Was an out-of-line call in val.c on every comparison. */
static inline int val_equal(Val a, Val b) {
    if (a == b)
        return 1;
    if (val_tag(a) == TAG_STRING && val_tag(b) == TAG_STRING) {
        HeapString *sa = val_as_string(a);
        HeapString *sb = val_as_string(b);
        return sa->len == sb->len && memcmp(sa->data, sb->data, (size_t)sa->len) == 0;
    }
    return 0;
}

/* Float discrimination: the top 16 bits fall outside the tag range. All
 * tags live in the negative-NaN exponent zone (see ta.h), which finite
 * doubles cannot reach (exponent <= 0x7FE ⇒ top 16 bits <= 0xFFEF) and
 * which -Inf (top 16 bits 0xFFF0) stays below of — so every finite double,
 * both infinities and the canonical NaN are floats, and nothing but a
 * genuinely tagged value matches the range. */
static inline int val_is_float(Val v) {
    uint16_t t = val_tag(v);
    return t < TAG_FIRST || t > TAG_LAST;
}

/* Convenience: get HeapPair* from a TAG_PAIR Val */
static inline HeapPair *val_as_pair(Val v) {
    return (HeapPair *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

/* Pair accessors (LOP_CAR/LOP_CDR). Inlined for the same
 * reason as val_is_true: list walking is the hottest non-arithmetic traffic
 * in the dispatch loop, and the bodies are a mask plus one load. Callers
 * keep their own tag checks: op_car/op_cdr only reach here for a TAG_PAIR —
 * car(nil) is a 'cartype death, not nil (issue #236). */
static inline Val val_get_car(Val v) { return val_as_pair(v)->car; }

static inline Val val_get_cdr(Val v) { return val_as_pair(v)->cdr; }

/* Tuple accessors (LOP_TUPLE_REF). Same inline discipline as the pair ones:
 * the field read is a mask plus one indexed load, and callers keep their own
 * tag check (a non-tuple reaching val_as_tuple is a bug, not a nil). */
static inline int val_is_tuple(Val v) { return val_tag(v) == TAG_TUPLE; }

static inline HeapTuple *val_as_tuple(Val v) {
    return (HeapTuple *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

static inline int val_tuple_len(Val v) { return val_as_tuple(v)->len; }

static inline Val val_tuple_get(Val v, int i) { return val_as_tuple(v)->slot[i]; }

/* Get HeapClosure* from a TAG_CLOS Val */
static inline HeapClosure *val_as_clos(Val v) {
    return (HeapClosure *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

/* 函数值 → 入口 id：字节码后端的 fn_entry/fn_args/fn_maxd 下标（原生后端
 * 叫 stage id），非函数值返回 -1。函数值有两种物理形态——载荷是 fnid 本身
 * （TAG_CLOS_ID，无自由变量：顶层函数引用/RT_CLOS，零堆分配）还是
 * HeapClosure*（TAG_CLOS，入口在 ->entry）——**只有这一处知道**。两个后端
 * 的四个调用点（CALL_COMMON / op_tcall / proc_spawn_begin / rt_stage_of）
 * 共用它，免得每个点各自把两种形态再判一遍。
 * TAG_NATIVE 不在此列：它的载荷是 cfunc 下标，走 ccall 而不是 fn 表。 */
static inline long val_closure_entry(Val v) {
    uint16_t tag = val_tag(v);
    if (tag == TAG_CLOS_ID)
        return (long)(v & 0x0000FFFFFFFFFFFFULL);
    if (tag == TAG_CLOS)
        return (long)val_as_clos(v)->entry;
    return -1;
}

/* Int constructor/accessor and the int↔double conversions. All five are
 * called on every execution of the arithmetic opcodes (OP_ADD/SUB/MUL/DIV:
 * int+int needs val_int/val_get_int, any float operand needs
 * val_to_double/val_from_double), so they must inline into the dispatch
 * loop — as out-of-line calls they cost 3-4 real `bl`s per OP_ADD. Each
 * encoding is written out here rather than delegating to val.c's internal
 * helpers so one operation stays a handful of register ops.
 *
 * These were out-of-line functions in val.c; val_int/val_get_int/
 * val_from_double/val_to_double/val_get_float keep their semantics exactly
 * (pure move, no behaviour change). */

static inline Val val_int(int64_t i) {
    /* Store as sign-extended int48 in the low 48 bits. Cast via union to
     * avoid UB on signed shift. */
    union {
        int64_t s;
        uint64_t u;
    } u;
    u.s = i;
    return ((uint64_t)TAG_INT << 48) | (u.u & 0x0000FFFFFFFFFFFFULL);
}

static inline int64_t val_get_int(Val v) {
    union {
        uint64_t u;
        int64_t s;
    } u;
    u.u = v & 0x0000FFFFFFFFFFFFULL;
    /* Sign-extend from 48 bits */
    if (u.u & 0x800000000000ULL)
        u.u |= 0xFFFF000000000000ULL;
    return u.s;
}

static inline double val_get_float(Val v) {
    union {
        uint64_t u;
        double d;
    } u;
    u.u = v;
    return u.d;
}

/* Widening for mixed arithmetic/comparison; see the contract note in ta.h.
 * Only int and float are valid inputs — any other type degrades to 0.0. */
static inline double val_to_double(Val v) {
    if (val_is_float(v))
        return val_get_float(v);
    if (val_is_int(v))
        return (double)val_get_int(v);
    return 0.0;
}

/* Deliberately NEVER narrows back to int: any arithmetic result that
 * involved a float stays a float, so `1.0 + 2` yields 3.0, not 3. This is
 * the bit copy val_float performs (val.c) — inlined so the float path of
 * the arithmetic opcodes needs no call at all. */
static inline Val val_from_double(double d) {
    union {
        double d;
        uint64_t u;
    } u;
    u.d = d;
    /* NaN bit patterns collide with the tag zone (top 16 bits 0xFFF1 and
     * above, see ta.h), so they are canonicalized at box time. -Inf needs no
     * fix-up: its top 16 bits are 0xFFF0, below TAG_FIRST. */
    if (u.u << 1 > 0xFFE0000000000000ULL) /* exponent all-ones, mantissa != 0 */
        u.u = VAL_CANON_NAN;
    return u.u;
}

/* Numeric-operand test for the comparison opcodes: the strict numeric tower
 * is int/float (issue #92). int vs float is numeric here (3 == 3.0 → true),
 * but any other type (string/symbol/pair/closure/bytes/pid/nil/bool) is not.
 * The comparison opcodes take their double-precision path only when one
 * operand is a float and the other is numeric — val_to_double maps every
 * non-numeric to 0.0, so without the numeric half of that gate
 * `"str" == 0.0` (and `"str" <= 0.0`) would be true. int/int keeps the
 * integer fallback path (val_equal / int compare), so arithmetic hot loops
 * execute the same code as before this gate.
 *
 * The disjunctive form is chosen over `num(a) && num(b) &&
 * (float(a) || float(b))` so the hot int/int path costs exactly 2 tag tests:
 * val_is_float(a) is false for an int, so the first conjunct fails and the
 * whole expression short-circuits before touching b.
 *
 * Semantic equivalence with the conjunctive form: val_is_float(x) implies
 * num(x) (a float is numeric), so
 *   (float(a) && num(b)) || (float(b) && num(a))
 *  ≡ (num(a) && num(b)) && (float(a) || float(b)).
 *
 * Short-circuit correctness on the non-double paths:
 *  - int/int: val_is_float(a) is false (tagged 0xFF..) ⇒ first disjunct
 *    fails on its first test; second disjunct's val_is_float(b) is likewise
 *    false ⇒ false after 2 tests, callers keep val_equal / int compare.
 *  - float + non-numeric (say a=float, b=string): first disjunct's
 *    num(b) is false for a string, and second disjunct's val_is_float(b) is
 *    false ⇒ false. No non-numeric operand can reach val_to_double, so
 *    `"str" == 0.0` stays false.
 *
 * Shared by the comparison paths in src/vm.c (LOP_EQ/LOP_LT/...);
 * inlined because it sits on every comparison in the dispatch loop. */
static inline int cmp_numeric_path(Val a, Val b) {
    return (val_is_float(a) && (val_is_int(b) || val_is_float(b))) ||
           (val_is_float(b) && (val_is_int(a) || val_is_float(a)));
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
    static atomic_int cached = -1; /* -1 = not parsed yet */
    int value = atomic_load(&cached);
    if (value < 0) {
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
        int expected = -1;
        atomic_compare_exchange_strong(&cached, &expected, cap);
        value = atomic_load(&cached);
    }
    return value;
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
    if (p->mem == NULL || p->heap_ptr > 0)
        return -1;
    int cap = ta_arena_cap_env();
    int want = p->mem_size;
    while (want < need && want < cap)
        want *= 2;
    if (want > cap)
        want = cap;
    if (want < need)
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
    proc_set_mem(p, new_mem, want);
    return 0;
}

/* ============================================================
 * Lazy heap allocation
 * ============================================================ */

/* Lazily allocate the TA_PROC_HEAP0 idling buffer on first use. It holds only
 * the stack, and it IS the initial heap size — the first heap object needs no
 * reservation, and from then on the heap grows through a collection
 * (proc_heap_alloc), never through a plain relocation: the idling buffer must
 * never hold an object that a relocation would invalidate. gc_to is NOT
 * allocated here — idle actors (blocked on recv) pay ~0 extra bytes. */
static inline int proc_heap_empty(Proc *p) { return p->heap_ptr == 0; }

static inline void proc_ensure_heap(Proc *p) {
    if (p->mem == NULL) {
        /* heap_ptr and gc_to stay 0/NULL: no objects, no collection yet */
        proc_set_mem(p, calloc(1, TA_PROC_HEAP0), TA_PROC_HEAP0);
    }
}

/* ============================================================
 * Inline helpers — stack access
 * ============================================================ */

static inline Val *proc_stack(Proc *p) { return p->stk; }

/* Deepest stack slot the heap must stay above: the live top (sp) or the
 * floor of the deepest reserved frame, whichever reaches lower. A frame is
 * reserved whole at CALL/RESERVE time but filled by pushes spread over
 * many instructions; with the heap-side gap measured against sp alone, a
 * mid-frame allocation could grow the heap into the region those later
 * pushes occupy. All fit checks and collection sizing go through this. */
static inline int proc_stack_lo(const Proc *p) {
    return p->sp < p->stack_floor ? p->sp : p->stack_floor;
}

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
        /* The slot moves with the arena's new top: recompute off against the
         * new mem_size. Nothing to re-check — a successful grow leaves
         * heap_ptr back at 0 with mem_size large enough for this slot. */
        off = p->mem_size + (p->sp - 1) * (int)sizeof(Val);
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
    /* Record the frame floor before the gap check: from here on every
     * heap-side fit must keep the heap above it (proc_stack_lo). Monotone
     * — frames pop, but the floor never rises: a stale floor only confines
     * the heap a little longer, never wrongly admits it. */
    if (lo_idx < p->stack_floor)
        p->stack_floor = lo_idx;
    if (p->mem_size + lo_idx * (int)sizeof(Val) < p->heap_ptr) {
        /* Heap non-empty: relocation must go through a collection (same
         * discipline as proc_push). The TA stack is the whole
         * root set here — OP_CALL publishes sp before reserving — so the
         * live set survives the move; deep-recursion workloads (compilers)
         * otherwise wedge at a small arena: their frames grow the stack
         * faster than heap-alloc-side collection grows the arena. */
        if (proc_arena_grow(p, (1 - lo_idx) * (int)sizeof(Val)) != 0 &&
            gc_collect(p, (1 - lo_idx) * (int)sizeof(Val)) != 0)
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
    static atomic_int cached = -1; /* -1 = not parsed yet */
    int value = atomic_load(&cached);
    if (value < 0) {
        int parsed = 0;
        const char *s = getenv("TA_GC_STRESS");
        if (s && *s) {
            char *end = NULL;
            errno = 0;
            long v = strtol(s, &end, 10);
            if (errno == 0 && end != s && *end == '\0' && v >= 1) {
                if (v > 1000000L)
                    v = 1000000L; /* clamp absurd values */
                parsed = (int)v;
            }
        }
        int expected = -1;
        atomic_compare_exchange_strong(&cached, &expected, parsed);
        value = atomic_load(&cached);
    }
    return value;
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
     * exists to prevent. Together with the per-instruction check in the
     * run loop, this turns a missed exit into a deterministic crash under
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
 * 工作表扩容 —— 值树遍历的显式栈
 * ============================================================
 *
 * 遍历不用递归：树深由用户数据决定（深列表能把 C 栈打爆），而且拷贝在关了
 * GC 门之后必须把中间结果放在 TA 栈之外。四处遍历（深拷 / 大小预估 /
 * 消息分片 / 资源收集）原先各抄了一份逐字相同的扩容，现在只有这一份。
 *
 * TA_PUSH 是全仓唯一的宏：元素类型随遍历而变（Val / CopyTask / 资源指针），
 * 函数收不下。
 */
static inline void ta_arr_grow(void **items, int *cap, int count, int need, size_t size) {
    if (count + need <= *cap)
        return;
    /* 起步 64 是实测值：从 8 起步的翻倍次数翻倍，每条消息多 10~20 次 realloc，
     * 消息吞吐掉 30%（临时表，1KB 的浪费换掉整串 realloc）。 */
    int next = *cap ? *cap : 64;
    while (next < count + need)
        next *= 2;
    void *grown = realloc(*items, (size_t)next * size);
    if (!grown)
        abort();
    *items = grown;
    *cap = next;
}

#define TA_PUSH(arr, count, cap, v)                                                                \
    (ta_arr_grow((void **)&(arr), &(cap), (count), 1, sizeof *(arr)), (arr)[(count)++] = (v))

/* ============================================================
 * 值树的形状 —— 「有几个孩子、第 i 个是谁、占多少字节」
 * ============================================================
 *
 * 消息分片（scheduler.c frag_copy）、深拷（val.c val_deep_copy /
 * val_converge_copy）、大小预估（val_tree_size）、
 * 资源收集（resource.c）原先各写了一遍 tag 分发：同一份结构知识六份副本，
 * 加一个堆类型要改六处，漏一处不是崩溃而是**静默少拷一个孩子**。现在只在
 * 这里。尺寸（val_node_shape → heap_obj_raw_size）同理只有一份。
 *
 * 「是容器」与「有几个孩子」必须分开：空 tuple / nfree==0 的闭包是容器、
 * 孩子数为 0，拷贝仍需一条 post 任务把它装配起来。
 */
/* 载荷是堆对象的 6 个 tag；其余 tag 的载荷是立即数（int48 / 符号下标 /
 * fnid / cfunc 下标）。 */
static inline int val_is_heap(Val v) {
    uint16_t tag = val_tag(v);
    return tag == TAG_PAIR || tag == TAG_CLOS || tag == TAG_STRING || tag == TAG_BYTES ||
           tag == TAG_RESOURCE || tag == TAG_TUPLE;
}

/* 载荷 → 通用堆指针（val_is_heap 为真时有效） */
static inline void *val_heap_ptr(Val v) { return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL); }

/* 形状 / 尺寸 / 骨架三处遍历撞到未知 tag 或 type 时走同一个口子：静默当叶子
 * 会让消息树少一半（docs/c-resource-design.md §8）。写成独立的 noreturn 函数
 * 而不是就地 fprintf+abort，是为了让下面几个热循环保持小巧 —— 带着 fprintf
 * 的 switch 过不了内联阈值，每个节点就多一次真实调用（实测 20% 吞吐）。 */
__attribute__((noreturn)) static inline void ta_unknown_heap_shape(const char *who, int what) {
    fprintf(stderr, "%s: unknown heap tag/type 0x%x\n", who, what);
    abort();
}

/* 尺寸公式的唯一一份：按 HeapHeader->type 分发。gc.c 的 gc_obj_raw_size 是
 * 它的指针版入口（resource.c 的空间扫描 / GC 只有裸指针）。写在头里是为了
 * 内联进拷贝与预估的热循环 —— 每条消息的每个节点都要问一次，一次真实调用
 * 就够了（消息吞吐是一次 send/recv 一个节点级成本）。 */
static inline int heap_obj_raw_size(const HeapHeader *h) {
    switch (h->type) {
    case HEAP_PAIR:
        return (int)sizeof(HeapPair);
    case HEAP_CLOS:
        return (int)sizeof(HeapClosure) + ((const HeapClosure *)h)->nfree * (int)sizeof(Val);
    case HEAP_STRING:
        return (int)sizeof(HeapString) + ((const HeapString *)h)->len + 1;
    case HEAP_BYTES:
        return (int)sizeof(HeapBytes) + ((const HeapBytes *)h)->len;
    case HEAP_RESOURCE:
        return (int)sizeof(HeapResource); /* 固定：header + owner_pid + res 指针 */
    case HEAP_TUPLE:
        return (int)sizeof(HeapTuple) + ((const HeapTuple *)h)->len * (int)sizeof(Val);
    default:
        ta_unknown_heap_shape("heap_obj_raw_size", h->type); /* 步进错一拍 = 把堆读成垃圾 */
    }
}

/* 一个节点的形状，一次问全（拷贝路径的内循环入口）：
 *   *nkids  孩子数（叶子为 0）
 *   返回    孩子的连续视图 —— pair 的 car/cdr 相邻、tuple 的 slot[]、closure
 *           的 free[] 都是紧密的 Val 数组；叶子返回 NULL。顺序即构造顺序。
 *   *raw    裸字节数（分片布局的原料；堆内账再过一次 ta_heap_object_size）。
 *           NULL = 这一路不需要尺寸（只问孩子在哪），内联后整段尺寸计算被
 *           DCE 掉 —— 传个哑元不如传 NULL 实在。
 *   dst     非 NULL 时顺手把骨架落上去：结构字段照抄、flags 清零（源堆里的
 *           GC 标记进了分片没有意义）、容器孩子槽留空等装配任务填。
 *           NULL = 只问形状（大小预估 / 资源收集）。
 *
 * 三个问题一次答完（各自的代价见上面每处的注释）：它们每个节点都会被问，
 * 拆成三次分发就多几次间接跳转。gc.c 只有裸指针、用不了 tag，那边仍走
 * heap_obj_raw_size。 */
__attribute__((always_inline)) static inline const Val *val_node_shape(void *dst, Val src,
                                                                       int *nkids, int *raw) {
    uint16_t tag = val_tag(src); /* 分发用 tag（寄存器里就有），不回头读 hdr->type：
                                  * 依赖 load + 跳表每个节点都要多付一次，实测 8%。 */
    const HeapHeader *sh = val_heap_ptr(src);
    HeapHeader *dh = dst;

    if (tag == TAG_PAIR) {
        /* car/cdr 由装配任务写。 */
        if (dh) {
            dh->type = HEAP_PAIR;
            dh->flags = 0;
        }
        *nkids = 2;
        if (raw)
            *raw = (int)sizeof(HeapPair);
        return &((const HeapPair *)sh)->car;
    }
    if (tag == TAG_TUPLE) {
        const HeapTuple *s = (const HeapTuple *)sh;
        if (dh) {
            HeapTuple *d = (HeapTuple *)dh;
            d->hdr.type = HEAP_TUPLE;
            d->hdr.flags = 0;
            d->len = s->len;
        }
        *nkids = s->len;
        if (raw)
            *raw = (int)sizeof(HeapTuple) + s->len * (int)sizeof(Val);
        return s->slot;
    }
    if (tag == TAG_CLOS) {
        const HeapClosure *s = (const HeapClosure *)sh;
        if (dh) {
            HeapClosure *d = (HeapClosure *)dh;
            d->hdr.type = HEAP_CLOS;
            d->hdr.flags = 0;
            d->entry = s->entry;
            d->nfree = s->nfree;
        }
        *nkids = s->nfree;
        if (raw)
            *raw = (int)sizeof(HeapClosure) + s->nfree * (int)sizeof(Val);
        return s->free;
    }
    if (tag == TAG_STRING) {
        const HeapString *s = (const HeapString *)sh;
        if (dh) {
            HeapString *d = (HeapString *)dh;
            d->hdr.type = HEAP_STRING;
            d->hdr.flags = 0;
            d->len = s->len;
            memcpy(d->data, s->data, (size_t)s->len + 1);
        }
        *nkids = 0;
        if (raw)
            *raw = (int)sizeof(HeapString) + s->len + 1;
        return NULL;
    }
    if (tag == TAG_BYTES) {
        const HeapBytes *s = (const HeapBytes *)sh;
        if (dh) {
            HeapBytes *d = (HeapBytes *)dh;
            d->hdr.type = HEAP_BYTES;
            d->hdr.flags = 0;
            d->len = s->len;
            memcpy(d->data, s->data, (size_t)s->len);
        }
        *nkids = 0;
        if (raw)
            *raw = (int)sizeof(HeapBytes) + s->len;
        return NULL;
    }
    if (tag == TAG_RESOURCE) {
        /* payload 在堆外，指针照抄即共享；归属也照抄（校验处已确认 == 发送
         * 方，入队成功后 mbox_deliver 再统一改写）。 */
        const HeapResource *s = (const HeapResource *)sh;
        if (dh) {
            HeapResource *d = (HeapResource *)dh;
            d->h.type = HEAP_RESOURCE;
            d->h.flags = 0;
            d->owner_pid = s->owner_pid;
            d->res = s->res;
        }
        *nkids = 0;
        if (raw)
            *raw = (int)sizeof(HeapResource); /* 固定：header + owner_pid + res 指针 */
        return NULL;
    }
    /* 立即数（int48 / nil / 布尔 / 符号 / pid / fnid / cfunc 下标 / 浮点）。 */
    *nkids = 0;
    if (raw)
        *raw = 0;
    if (val_is_float(src) || tag == TAG_INT || tag == TAG_NIL || tag == TAG_TRUE ||
        tag == TAG_FALSE || tag == TAG_SYM || tag == TAG_PID || tag == TAG_CLOS_ID ||
        tag == TAG_NATIVE)
        return NULL;
    ta_unknown_heap_shape("val_node_shape", tag); /* 不许静默当叶子 */
}

/* ============================================================
 * Chunk arena — the C callback's private heap (gate closed)
 *
 * While the GC gate is closed every proc_heap_alloc made by a running C
 * callback lands here: a bump allocator over a chain of chunks. The arena
 * object itself is a local of do_ccall's frame (Proc.ccall points at it for
 * the duration of the call), so no part of it is per-actor state and the
 * first chunk's storage is that frame's own buffer. Two properties make this
 * the whole safety story for C modules (issue #160):
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
 * OP_CCALL exit the result converges into the heap (callback_arena_converge)
 * and the chain releases its malloc'd chunks; the arena then dies with the
 * call's frame, so chunk pointers cannot outlive it.
 * ============================================================ */

/* Malloc'd capacity for overflow chunks. Tunable: callbacks whose results
 * outgrow TA_CCALL_CHUNK0 pay one malloc per TA_CCALL_CHUNK_NEXT bytes. */
#ifndef TA_CCALL_CHUNK_NEXT
#define TA_CCALL_CHUNK_NEXT 4096
#endif

/* Target gap between heap top and stack bottom after any arena growth or
 * collection (proc_reserve_heap / proc_heap_alloc / gc sizing). Established
 * where growth happens — there is no periodic check: stack-side pushes are
 * guarded per push by proc_push, frame entries by proc_stack_reserve's gap
 * check (#248). */
#ifndef TA_STACK_HEADROOM
#define TA_STACK_HEADROOM 256
#endif

/* Minimum gap the heap-side fit checks must leave below the stack bottom
 * (min(sp, stack_floor) — the live top or the deepest reserved frame
 * floor). A handler that grows the heap (a deep message copy, a long
 * string) spends that margin inside the opcode — the fit checks are what
 * stop it from spending the last slots. Two slots: one for the handler's
 * net one-slot push after a check, one spare so a second push in the same
 * handler still lands above the heap (proc_push compares each push against
 * the heap top, but with objects present the arena cannot move — a failed
 * comparison is fatal, so legal pushes must always win it; #232 was a deep
 * copy that exactly fit, consumed by a push). */
#ifndef TA_HEAP_FIT_SLACK
#define TA_HEAP_FIT_SLACK 16
#endif

/* Guarantee room for `bytes` of heap objects (plus stack headroom) before
 * a gate-closed rebuild whose size is known up front — deep-copying an
 * incoming message, a spawned closure, a callback result. The copy itself
 * cannot grow the arena (its worklist holds unrooted Vals), so the room
 * must exist beforehand. The gate must be open here: growth is a
 * collection rooted at the TA stack (or a plain reservation while the
 * heap is empty). */
static inline void proc_reserve_heap(Proc *p, int bytes) {
    if (p->mem_size - p->heap_ptr + proc_stack_lo(p) * (int)sizeof(Val) >=
        bytes + TA_HEAP_FIT_SLACK)
        return;
    if (proc_heap_empty(p)) {
        /* The arena must additionally hold the stack already in place (a
         * deep stack on an empty heap), the incoming bytes, and headroom —
         * otherwise growth reports "large enough" and the copy collides
         * with the stack. Floor-aware: the copy must also clear the deepest
         * reserved frame. */
        int stack_bytes = -proc_stack_lo(p) * (int)sizeof(Val);
        if (proc_arena_grow(p, p->heap_ptr + stack_bytes + bytes + TA_STACK_HEADROOM) != 0)
            ta_arena_fatal(p, "cannot reserve arena room for an incoming copy");
    } else if (gc_collect(p, bytes + TA_STACK_HEADROOM) != 0) {
        ta_arena_fatal(p, "incoming data does not fit: heap + stack exceed the arena");
    }
}

/* Open the arena: the first chunk is the caller's own buffer, so a callback
 * that stays within TA_CCALL_CHUNK0 never mallocs. */
static inline void callback_arena_init(CallbackArena *a) {
    a->head.next = NULL;
    a->head.used = 0;
    a->head.cap = TA_CCALL_CHUNK0;
    a->cur = &a->head;
    a->total = 0;
    a->resources = 0;
    a->converge = 0;
    a->promote = 0;
}

/* Bump-allocate from the tail chunk, retiring it and opening a fresh one when
 * the request does not fit (oversized objects get a chunk of their own).
 * Chunks are never realloc'd or moved. Returns NULL if that malloc fails — the
 * caller decides whether that is fatal. */
static inline void *callback_arena_alloc(CallbackArena *a, int size) {
    TaChunk *c = a->cur;
    if (c->used + size > c->cap) {
        int cap = size > TA_CCALL_CHUNK_NEXT ? size : TA_CCALL_CHUNK_NEXT;
        TaChunk *nc = malloc(sizeof(TaChunk) + (size_t)cap);
        if (nc == NULL)
            return NULL;
        nc->next = NULL;
        nc->used = 0;
        nc->cap = cap;
        c->next = nc; /* c is always the tail of the chain */
        a->cur = c = nc;
    }
    uint8_t *data = (c == &a->head) ? a->buf : (uint8_t *)(c + 1);
    void *ptr = data + c->used;
    c->used += size;
    a->total += size;
    memset(ptr, 0, size);
    return ptr;
}

/* Release the arena's malloc'd chunks. Only called when nothing can reference
 * chunk objects any more (their data has converged into the heap, or the
 * callback yielded and its result is dropped) — chunk objects are not
 * individually tracked, the arena dies as a whole. The first chunk is the
 * caller's buffer: it is not ours to free. */
static inline void callback_arena_release(CallbackArena *a) {
    TaChunk *c = a->head.next;
    while (c != NULL) {
        TaChunk *next = c->next;
        free(c);
        c = next;
    }
    a->head.next = NULL;
    a->head.used = 0;
    a->cur = &a->head;
    a->total = 0;
}

/* Is this pointer into the callback arena? The first chunk is the callback
 * frame's buffer; malloc'd chunks live on their own. With no C call in flight
 * there is no arena, hence nothing in one. */
static inline int val_in_chunk(Proc *p, void *ptr) {
    CallbackArena *a = p->ccall;
    if (a == NULL)
        return 0;
    uint8_t *q = (uint8_t *)ptr;
    if (q >= a->buf && q < a->buf + TA_CCALL_CHUNK0)
        return 1;
    for (TaChunk *c = a->head.next; c != NULL; c = c->next)
        if (q >= (uint8_t *)(c + 1) && q < (uint8_t *)(c + 1) + c->used)
            return 1;
    return 0;
}

/* OP_CCALL exit: copy the callback's result from the chunk arena into the
 * heap, then free the arena. The gate may be closed here (vm.c converges
 * before reopening it); chunk addresses are stable regardless.
 *
 * Fast path — the heap has room for the whole arena plus headroom: a deep
 * copy that passes arena references through and rebuilds only chunk
 * objects. DAG sharing inside the result is preserved by forwarding
 * pointers (stored after the chunk object's header, cf. gc_copy_obj), so
 * each chunk object is rebuilt exactly once and a->total is an exact
 * bound for the copy; the headroom covers the caller's proc_push of the
 * result. Gate bump keeps collections out (the worklist holds unrooted
 * Vals) while a->converge routes the copy's own allocations to the
 * heap.
 *
 * Slow path — one collection with the chunk arena as a second source space:
 * reachable chunk objects promote through the normal Cheney scan, and
 * forwarding pointers rewrite every reference (gc_collect_converge).
 *
 * The pending-request is left undrained on the way out: the result is not
 * rooted until the caller pushes it. */
static inline void callback_arena_converge(Proc *p, Val *result) {
    CallbackArena *a = p->ccall;
    if (a->total == 0)
        return;
    int free_heap = p->mem_size + proc_stack_lo(p) * (int)sizeof(Val) - p->heap_ptr;
    if (free_heap >= a->total + TA_STACK_HEADROOM) {
        proc_gc_enter(p);
        a->converge = 1;
        *result = val_converge_copy(p, *result);
        a->converge = 0;
        callback_arena_release(a);
        proc_gc_leave(p);
    } else {
        gc_collect_converge(p, result);
        callback_arena_release(a);
    }
}

static inline void *proc_heap_alloc(Proc *p, int size) {
    /* Align to 8 bytes and reserve space for GC's forwarding pointer. */
    size = ta_heap_object_size(size);

    /* Gate closed (C callback): allocate from the chunk arena. This keeps
     * the arena itself frozen for the whole callback — no collection can
     * run, so every Val and raw pointer the callback holds stays valid —
     * and the result converges into the heap at OP_CCALL exit. Routed by
     * the callback window (p->ccall), not the gate: the gate also closes
     * inside the VM (message delivery, DOWN walks), whose allocations
     * belong in the heap. */
    if (p->ccall != NULL && !p->ccall->converge) {
        void *obj = callback_arena_alloc(p->ccall, size);
        if (obj == NULL)
            ta_arena_fatal(p, "out of memory allocating a callback chunk");
        return obj;
    }

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
    /* The idling buffer IS the initial heap size (proc_ensure_heap), so the
     * first object needs no reservation; and from here on growth goes through
     * collections (below), never reservations: a reservation would move
     * objects that Vals already point to. */
    /* Request a collection once the heap has outgrown the trigger, so the
     * live set stays well inside the arena, then collect in place if that is
     * safe right now (gate open). Must happen before the fit check: a
     * collection frees room the incoming object may need. */
    if (p->heap_ptr > p->gc_trigger)
        p->gc_pending = 1;
    proc_gc_drain(p);
    if (p->heap_ptr + size + TA_HEAP_FIT_SLACK >
        p->mem_size + proc_stack_lo(p) * (int)sizeof(Val)) {
        /* Out of room: compact the live set and grow the arena in one
         * collection (the live set + `size` land in a larger semispace).
         * The gap is measured against proc_stack_lo — the floor of the
         * deepest reserved frame, whose slots later pushes will fill (#248:
         * no periodic check exists, this fit is what keeps the heap out of
         * that region). TA_STACK_HEADROOM is the post-growth target margin
         * between heap top and stack bottom; TA_HEAP_FIT_SLACK keeps a
         * check itself from stranding the pushes that follow it (#232: a
         * deep copy that exactly fit was then consumed by a push). */
        if (p->gc_gate != 0 || gc_collect(p, size + TA_STACK_HEADROOM) != 0)
            ta_arena_fatal(p, "allocation does not fit: heap + stack exceed the arena");
    }
    void *ptr = p->mem + p->heap_ptr;
    p->heap_ptr += size;
    memset(ptr, 0, size);
    return ptr;
}

/* ---- 单调钟（毫秒）----
 * timer/net 的 deadline 数学与 io poller 的唤醒扫描共用。原是 net.c 的
 * net_now_ms（src/ 时代），net 下沉为 dylib 后时钟必须留在 core：dylib
 * 不得成为 core 的符号提供方（lib/time.c 曾跨边界引用它——依赖恰好被
 * RTLD_GLOBAL 掩盖的加载顺序，正误）。 */
static inline int64_t mono_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

#endif /* TA_INLINE_H */