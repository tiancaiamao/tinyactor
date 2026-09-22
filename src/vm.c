/*
 * vm.c — bytecode VM: scheduler, process lifecycle, opcode dispatch
 */

#include "ta.h"
#include <assert.h>
#include <dlfcn.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Thread-local current process — set by worker_loop before executing a proc */
__thread Proc *tls_current_proc = NULL;

/* Equality for OP_EQ/OP_NE. Strings compare by content (HeapString holds
 * len + NUL-terminated data); ints/symbols/nil/true/false compare by their
 * NaN-boxed value (int payload is direct, symbols are interned); everything
 * else (pair/closure/bytes/pid) compares by pointer identity. */
static int val_equal(Val a, Val b) {
    if (val_is_string(a) && val_is_string(b)) {
        HeapString *sa = val_get_string(a);
        HeapString *sb = val_get_string(b);
        return sa->len == sb->len && memcmp(sa->data, sb->data, sa->len) == 0;
    }
    return a == b;
}

/* Numeric-operand test for the comparison opcodes: the strict numeric tower
 * is int/float (issue #92). int vs float is numeric here (3 == 3.0 → true),
 * but any other type (string/symbol/pair/closure/bytes/pid/nil/bool) is not.
 * The comparison opcodes take their double-precision path only when one
 * operand is a float and the other is numeric (val_is_num) — val_to_double
 * maps every non-numeric to 0.0, so without the val_is_num half of that gate
 * `"str" == 0.0` (and `"str" <= 0.0`) would be true. int/int keeps the
 * integer fallback path (`val_equal` / int compare), so arithmetic hot loops
 * execute the same code as before this gate. */
static int val_is_num(Val v) { return val_is_int(v) || val_is_float(v); }

/* When the comparison opcodes take the double-precision path: both operands
 * numeric AND at least one a float (int/int stays on the integer fallback).
 *
 * The disjunctive form is chosen over `val_is_num(a) && val_is_num(b) &&
 * (val_is_float(a) || val_is_float(b))` so the hot int/int path costs
 * exactly 2 tag tests (same as pre-#159): `val_is_float(a)` is false for an
 * int, so the first conjunct of the first disjunct fails and the whole
 * expression short-circuits before touching b. The old form had to prove
 * `val_is_num` on BOTH operands (2 tests) before the float disambiguation
 * (2 more), i.e. 4 tests per int/int comparison.
 *
 * Semantic equivalence with the conjunctive form: `val_is_float(x)` implies
 * `val_is_num(x)` (a float is numeric), so
 *   (float(a) && num(b)) || (float(b) && num(a))
 *  ≡ (num(a) && num(b)) && (float(a) || float(b))
 * — the left disjunct asserts a-float + b-numeric (⇒ both numeric, a float);
 * the right asserts b-float + a-numeric (⇒ both numeric, b float). Their
 * disjunction is exactly "both numeric and at least one float".
 *
 * Short-circuit correctness on the non-double paths:
 *  - int/int: val_is_float(a) is false (tagged 0xFF..) ⇒ first disjunct
 *    fails on its first test; second disjunct's val_is_float(b) is likewise
 *    false ⇒ false after 2 tests, callers keep val_equal / int compare.
 *  - float + non-numeric (say a=float, b=string): first disjunct's
 *    val_is_num(b) = is_int(b)||is_float(b) is false for a string, and
 *    second disjunct's val_is_float(b) is false ⇒ false. No non-numeric
 *    operand can reach val_to_double, so `"str" == 0.0` stays false. */
static int cmp_numeric_path(Val a, Val b) {
    return (val_is_float(a) && val_is_num(b)) || (val_is_float(b) && val_is_num(a));
}

/* ================================================================
 * Yield API — clean interface for C functions to suspend the
 * current proc.  Replaces the old 'would-block magic symbol.
 * ================================================================ */
void vm_watch_fd(VM *vm, int fd, short events) {
    (void)vm;
    Proc *p = tls_current_proc;
    p->wait_fd = fd;
    p->wait_events = events;
}

void vm_yield(VM *vm) {
    (void)vm;
    /* Per-proc flag: multiple worker threads share one VM and call C
     * functions concurrently; a shared flag on VM would race across
     * threads (a spurious yield in one proc, or a lost yield in another). */
    Proc *p = tls_current_proc;
    if (p)
        p->yield_requested = 1;
}

void vm_die(VM *vm, const char *reason) {
    /* Same per-proc discipline as vm_yield (see above). OP_CCALL_NAME
     * checks die_requested when the C function returns and hands the
     * calling proc to proc_die — a builtin's equivalent of the opcode
     * type errors (cartype/cdrtype/divzero): die loudly near the cause
     * instead of returning a sentinel that propagates (issue #101). */
    Proc *p = tls_current_proc;
    if (p) {
        p->die_requested = 1;
        p->die_reason = val_symbol(vm_intern_symbol(vm, reason));
    }
}
/* ================================================================
 * Stack walking & function-name resolution
 *
 * Shared by the sampling profiler (prof.c) and the crash report
 * (proc_die in scheduler.c). Frame layout is defined by OP_CALL /
 * OP_TAIL_CALL below.
 * ================================================================ */

/* pc -> owning fn_id via binary search over fn_table (sorted offsets). */
static int vm_fn_of_pc(const Proc *p, int pc) {
    int lo = 0, hi = p->fn_count - 1, ans = 0;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (p->fn_table[mid] <= pc) {
            ans = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return ans;
}

/* Walk the TA call stack. Frame layout (see OP_CALL / OP_TAIL_CALL in vm.c):
 *   st[fp+0..]     args (arg_j at fp+j), then the callee's free vars
 *   st[fp-5..]     locals + temporaries (the frame body, below the header)
 *   st[fp-1]       closure
 *   st[fp-2]       ret_pc  (-1 sentinel in the root frame)
 *   st[fp-3]       old_fp  (caller's fp; fp grows *more negative* with
 *                           depth, so a caller always has old_fp > fp)
 *   st[fp-4]       caller_sp
 * Returns depth; out[] filled leaf..root (current fn first, outermost last). */
int vm_walk_stack(const VM *vm, const Proc *p, int *out, int max_depth) {
    int depth = 0;
    int fp = p->fp;
    int mem_words = p->mem_size / (int)sizeof(Val);
    const Val *st = (const Val *)(p->mem + p->mem_size);

    out[depth++] = vm_fn_of_pc(p, p->pc);
    while (depth < max_depth) {
        /* frame header must be within the stack */
        if (fp - 4 < -mem_words)
            break;
        Val rv = st[fp - 2];
        Val of = st[fp - 3];
        if (!val_is_int(rv) || !val_is_int(of))
            break;
        int ret_pc = (int)val_get_int(rv);
        int old_fp = (int)val_get_int(of);
        if (ret_pc < 0) /* root frame sentinel */
            break;
        if (ret_pc >= vm->code_len)
            break;
        if (old_fp <= fp) /* caller fp must be greater */
            break;
        out[depth++] = vm_fn_of_pc(p, ret_pc);
        fp = old_fp;
    }
    return depth;
}

/* fn_id -> fn name (.tabc v2 name table), or NULL if unknown (v1 module).
 * The caller may fall back to "fn#<id>" when this returns NULL. */
const char *vm_fn_name(const VM *vm, int fid) {
    if (vm->fn_names && fid >= 0 && fid < vm->fn_names_count && vm->fn_names[fid])
        return vm->fn_names[fid];
    return NULL;
}
/* ================================================================
 * print_val
 * ================================================================ */
void print_val(VM *vm, Val v) {
    if (val_is_int(v)) {
        printf("%lld", (long long)val_get_int(v));
    } else if (val_is_float(v)) {
        /* %g: shortest decimal that round-trips; 3.14 → "3.14", 2.5 → "2.5",
         * 3.0 → "3", 1.0/0.0 → "inf". */
        printf("%g", val_get_float(v));
    } else if (val_is_nil(v)) {
        printf("nil");
    } else if (v == val_true()) {
        printf("true");
    } else if (v == val_false()) {
        printf("false");
    } else if (val_is_symbol(v)) {
        printf("%s", vm->symbols[val_get_symbol(v)]);
    } else if (val_is_string(v)) {
        HeapString *s = val_get_string(v);
        printf("%.*s", s->len, s->data);
    } else if (val_is_pair(v)) {
        printf("(");
        print_val(vm, val_get_car(v));
        Val rest = val_get_cdr(v);
        while (val_is_pair(rest)) {
            printf(" ");
            print_val(vm, val_get_car(rest));
            rest = val_get_cdr(rest);
        }
        if (!val_is_nil(rest)) {
            printf(" . ");
            print_val(vm, rest);
        }
        printf(")");
    } else if (val_is_pid(v)) {
        printf("<pid %d>", (int)val_get_pid(v));
    } else {
        printf("?");
    }
}

/* Reverse `n` stack slots st[base .. base+n-1] (base is a proc_stack index,
 * negative inside a frame) in place. The calling convention pushes the
 * closure first then arg0..argN-1, so on the stack the args read
 * argN-1..arg0 top-down; reversing turns them into the callee frame's
 * forward order (arg_j at fp+j) — no C buffer, bytecode unchanged. */
static void stack_reverse(Val *st, int base, int n) {
    for (int i = 0, j = n - 1; i < j; i++, j--) {
        Val t = st[base + i];
        st[base + i] = st[base + j];
        st[base + j] = t;
    }
}

/* ================================================================
 * Opcode dispatch (perf series phase A, spec §A) — computed goto, one impl
 *
 * There is exactly one dispatch implementation below, written out in full
 * with no dispatch macros:
 *
 *   CASE_OP_ADD:                <- one label per opcode, reachable only
 *       ...                        through dispatch_table[]
 *       TICK_FETCH();           <- per-instruction bookkeeping (see below)
 *       if (op >= OP_COUNT)
 *           goto CASE_OP_UNKNOWN;
 *       goto *dispatch_table[op];
 *
 * dispatch_table[] maps opcode -> handler label address (`&&CASE_OP_xxx`), so
 * that tail jump is an indirect jump through a link-time-resolved table:
 * computed goto, the classic Lua interpreter win. Because the jump is spelled
 * out at every handler instead of shared, the CPU's branch predictor can
 * learn each opcode's successor separately — where `switch` compiles the
 * whole dispatch to ONE indirect branch serving all opcodes.
 *
 * That duplication is the mechanism, not boilerplate to be factored back into
 * a NEXT() macro: how many indirect branch points survive is exactly what a
 * compiler's tail-merge/tail-duplication pass decides, and it is visible in
 * the disassembly only if it is visible in the source.
 *
 * A second, `switch`-based backend (CASE()/NEXT()/DISPATCH() macros, picked by
 * a -D override plus a make-file shortcut) used to sit behind an #if — the same
 * 49 handlers under two spellings (49 labels, 54 `goto *dispatch_table[op]`
 * tails). It was deleted here:
 *   - reading or editing any handler first required working out which
 *     backend was expanded; the dispatch skeleton is the part we tune most,
 *     so it is the last place that should be behind an #if;
 *   - a macro-hidden jump is macro-hidden in the debugger, profiler and
 *     disassembly too (the switch form had no `goto` to step into at all);
 *   - the two forms were never behaviourally different: the switch one was
 *     pure duplicated surface for every test and mirror check to cover.
 *
 * The measurement history that motivated the old escape hatch is kept as the
 * standing lesson — the verdict is code-shape-dependent, not a property of
 * the compiler brand (same source, arm64, interleaved runs):
 *
 *   PR #154 (Apple clang 16, pre-#159 code shape) — clang tail-merged all
 *     duplicated jumps into ONE shared indirect branch, and the goto build
 *     measured ~3-5% SLOWER. Verdict then: exclude clang from the default.
 *   After #159's ta_inline.h inlining changed the code shape — tail
 *     duplication now recovers 3 indirect branch points in the goto build
 *     (switch: 1) — clang 16 measures FASTER with goto: mini dispatch
 *     microbench +16-19%, real tavm collatz 1M interleaved rounds
 *     10.50-10.54 s vs 10.78-10.87 s switch (~3%).
 *
 * Lesson: the verdict swings with code shape, not with the compiler brand, so
 * re-measure with interleaved runs whenever the VM body changes materially.
 * With the switch backend gone there is no build flag to reach for if a
 * future code shape flips the verdict again: the dispatch itself would have
 * to be changed, or the regression accepted on its merits. That is the price
 * of one implementation and one code path.
 *
 * Portability: labels-as-values is a GNU C extension, present in every
 * compiler this project supports (gcc, clang, Apple clang), and the wasm path
 * is covered too — emcc 5.0.0 compiles this file losslessly: `&&label`
 * becomes data-segment constants and the indirect goto degrades to a
 * loop-local `br_table`, i.e. to what the switch form was lowered to on that
 * target, so nothing is lost by not having a switch fallback there.
 * ================================================================ */

/* ---- hot-path stack access: the loop-local `sp` ------------------------
 *
 * `sp` is to the TA stack what `pc` is to the instruction stream: the
 * loop-local copy of the proc's state, declared once in vm_run_proc and
 * carried through the whole run. Handlers read and write slots through the
 * macros below instead of calling proc_push/proc_pop, which round-trips
 * p->sp through memory on every slot. proc_push did exactly two things per
 * slot, and neither may be dropped — only moved:
 *
 * 1. The stack/heap collision check. It no longer runs per slot. What it
 *    enforced — no live stack slot may reach down into the heap — is
 *    re-established once per instruction, at the boundary, by the room
 *    check in TICK_FETCH below: after a boundary the stack top sits at
 *    least TA_STACK_HEADROOM (8 KiB = 1024 slots) above the heap top, or
 *    TA_EMPTY_HEAP_SLACK slots above it while the heap is still empty.
 *    That is enough because between two boundaries a handler grows the net
 *    stack by at most one slot: every handler below pushes at most one
 *    value (SP_PUSH appears once per execution — where it appears twice,
 *    the two are exclusive branches — and the only handler pushing in a
 *    loop, OP_CONS, is net -1; OP_CCALL's yield path re-pushes exactly the
 *    operands it popped, net 0). Multi-slot growth reserves its whole
 *    range in one step through proc_stack_reserve instead: OP_ENTER the
 *    slots codegen reports as `nslots`, OP_CALL/OP_TAIL_CALL the new
 *    frame's header (nfree+3 slots below the operands).
 *
 *    Those reserves cover the frame region — the slots a call chain needs
 *    for its locals — and deliberately nothing more, because the operand
 *    stack is what the boundary check is for. Every operand pushed below
 *    the frame floor (the arguments of a call, until OP_CALL consumes
 *    them) leaves the reserved range again; a reserve that tried to cover
 *    operands would have to be as deep as the deepest call, which is not
 *    a per-function constant. Hence the invariant is two-sided: reserves
 *    take care of everything a single instruction may need, the boundary
 *    takes care of the accumulation. The spawn builtins (behind OP_BUILTIN)
 *    are the exception that shows the split: they push onto a *different*,
 *    fresh proc, through proc_push, check intact.
 *
 *    The empty heap gets its own, much smaller slack rather than
 *    TA_STACK_HEADROOM: nothing can be collected there, so
 *    proc_stack_headroom returns early, deliberately, and the boundary
 *    re-establishes the invariant with a plain reservation instead — one
 *    that cannot be undone. Sizing that at TA_STACK_HEADROOM would push
 *    every fresh (or idling-then-woken) actor into an 8 KiB block just for
 *    running a few opcodes, which is the regression the empty-heap
 *    shortcut exists to avoid. A small slack is sufficient, by the same
 *    one-slot-per-instruction argument as above.
 *
 *    Residual risk, of the same kind proc_push's fatal path already
 *    covered: a single instruction that executes deeper than it reserved —
 *    hand-written .tabc, never codegen output, which sizes `nslots` from
 *    the same per-function depth it uses for every LOAD/STORE, and emits
 *    operand pushes one instruction at a time — now writes past the heap
 *    top instead of dying in "stack and heap meet before the new frame
 *    fits". The boundary checks after an instruction, not inside one.
 *
 * 2. Publishing sp. Everything outside a handler that can move or size the
 *    arena reads p->sp: the collector's root scan (gc_collect_internal
 *    walks the live range [p->sp, 0)), the reservation helpers
 *    (proc_stack_reserve, proc_reserve_heap, proc_stack_headroom,
 *    proc_heap_alloc), proc_arena_grow's stack move, proc_chunk_converge,
 *    scheduler.c's mbox_pop / proc_die, and the allocation path reached
 *    from val_deep_copy. So every call below that can reach one of those
 *    publishes first — SP_PUBLISH() — and never earlier than the pushes it
 *    has to reflect: a value pushed just before a collection must be in
 *    p->sp already, or the collector scans past it, moves the object and
 *    leaves the raw pointer dangling on the stack. Only pushes need this:
 *    left too low, p->sp re-scans slots that still hold Vals (they were
 *    live a moment ago), which is why the pops below publish nothing.
 *    Nothing outside the handlers writes p->sp of a *running* proc
 *    (proc_new / the spawn builtins set it for a fresh child), so an
 *    ordinary call needs no reload afterwards; OP_CCALL and OP_BUILTIN are
 *    the exceptions — the callback / builtin owns p->sp while it runs — see
 *    there.
 *
 * Write-back points (p->sp = sp) are therefore: SP_PUBLISH() in the
 * handlers that call into the list above, the three exits — suspend (the
 * recv / recv_peek / recv_after builtins and OP_BUILTIN's rewind,
 * OP_CCALL yield), die (every proc_die
 * call site, vm_unknown_opcode included) and budget exhausted (TICK_FETCH)
 * — and the OP_CCALL / OP_BUILTIN boundary, where p->sp is both written
 * back before the call and reloaded after it. */
#define SP_PUSH(v) (*(Val *)(p->mem + (p->mem_size + (--sp) * 8)) = (v))
#define SP_POP() (*(Val *)(p->mem + (p->mem_size + (sp++) * 8)))
#define SP_PEEK(off) (*(Val *)(p->mem + (p->mem_size + (sp + (off)) * 8)))
#define SP_PUBLISH() (p->sp = sp)

/* Stack room the boundary keeps above the heap top while the heap is still
 * empty: the one state where proc_stack_headroom refuses to act (nothing to
 * collect), so the boundary reserves instead — see TICK_FETCH below. In
 * slots, and deliberately far below TA_STACK_HEADROOM (ta_inline.h): the
 * reservation cannot be undone, and a fresh actor must not be pushed into a
 * TA_STACK_HEADROOM-sized block just for running a few opcodes. One
 * instruction grows the net stack by at most one slot; 16 is that with room
 * to spare. */
#define TA_EMPTY_HEAP_SLACK 16

/* One instruction's bookkeeping plus the fetch of the following opcode: the
 * first half of every handler tail (TICK_FETCH() then the bounds-checked
 * indirect jump to the handler of the opcode just fetched). Order: sample with
 * the counter of the instruction just executed, spend one unit of reduction
 * budget, re-assert the GC gate, then fetch. The room check uses the
 * loop-local `sp` — the true stack top, see the macros above — and is where
 * the stack/heap collision test now lives, once per instruction: it leaves
 * the top at least TA_STACK_HEADROOM (heap non-empty: collect) or
 * TA_EMPTY_HEAP_SLACK slots (heap empty: reserve) clear of the heap top, and
 * one instruction cannot spend more than one slot of that. The check is split
 * in two because with nothing allocated yet proc_stack_headroom returns early
 * on purpose, so the room has to come from proc_stack_reserve, which grows the
 * arena outright — legal exactly while the heap is empty (proc_arena_grow
 * refuses otherwise), and cheap because nothing is there to be invalidated. */
#define TICK_FETCH()                                                                               \
    do {                                                                                           \
        if (prof_on && (r & 63) == 63) {                                                           \
            p->pc = pc;                                                                            \
            uint64_t now = prof_now_ns();                                                          \
            prof_collect(vm, p, now - prof_last);                                                  \
            prof_last = now;                                                                       \
        }                                                                                          \
        if (++r >= reductions) {                                                                   \
            p->pc = pc;                                                                            \
            p->sp = sp;                                                                            \
            return 0;                                                                              \
        }                                                                                          \
        assert(p->gc_gate == 0);                                                                   \
        if (p->heap_ptr > TA_PROC_CHUNK0) {                                                        \
            if (p->mem_size - p->heap_ptr + sp * 8 < TA_STACK_HEADROOM) {                          \
                p->sp = sp; /* proc_stack_headroom re-derives the free space from p->sp */         \
                proc_stack_headroom(p); /* safe point: stack is the whole root set */              \
            }                                                                                      \
        } else if (p->mem_size - p->heap_ptr + sp * 8 < TA_EMPTY_HEAP_SLACK * 8) {                 \
            p->sp = sp; /* the reservation derives the stack top from p->sp too */                 \
            proc_stack_reserve(p, sp - TA_EMPTY_HEAP_SLACK);                                       \
        }                                                                                          \
        op = p->code[pc++];                                                                        \
    } while (0)

/* Report an opcode with no handler and kill the proc. Reached from the
 * dispatch entry and from every handler tail's bounds check: an opcode >=
 * OP_COUNT has no dispatch_table[] entry to jump through, so it is reported
 * here instead of dereferencing the table. `pc` is the pc the opcode was
 * fetched from, hence the reported `pc - 1`; returns -1 to leave vm_run_proc,
 * like the other die paths. */
static int vm_unknown_opcode(VM *vm, Proc *p, uint8_t op, int pc) {
    fprintf(stderr, "vm_run_proc: unknown opcode %d at pc=%d\n", op, pc - 1);
    int badop = vm_intern_symbol(vm, "badopcode");
    p->pc = pc;
    proc_die(vm, p, val_symbol((uint32_t)badop));
    return -1;
}

int vm_run_proc(VM *vm, Proc *p, int reductions) {
    /* pc lives in a C local for the whole run and is written back to p->pc
     * only at the exit points (suspend / die / budget exhausted) and before
     * prof_collect, which resolves the leaf frame from p->pc. */
    int pc = p->pc;
    /* The stack pointer is the same kind of local, with the same discipline:
     * handlers move it through the SP_* macros above and publish it back
     * (SP_PUBLISH) at every point where something outside the handler reads
     * p->sp — the full list is in the comment on those macros. */
    int sp = p->sp;
    int prof_on = vm->prof_on;
    uint64_t prof_last = 0;
    if (prof_on)
        prof_last = prof_now_ns();

    /* Instruction counter: exactly one per executed instruction, shared by
     * the reduction budget and the sampling profiler (Lua count-hook style).
     * `op` is the dispatched opcode: every handler tail re-checks it against
     * OP_COUNT, and the unknown-opcode report names it. */
    int r = 0;
    uint8_t op;

    /* Opcode -> handler label. Address-of-label (`&&`) is a GNU C extension;
     * the table is static so it is built once. Every opcode in [0, OP_COUNT)
     * has a handler, so the table is total: the dispatch step needs only a
     * bounds check, and an out-of-range op is routed to CASE_OP_UNKNOWN. The
     * reserved actor numbers have entries too — their arms report them. */
    static const void *const dispatch_table[OP_COUNT] = {
        [OP_PUSH_NIL] = &&CASE_OP_PUSH_NIL,
        [OP_PUSH_TRUE] = &&CASE_OP_PUSH_TRUE,
        [OP_PUSH_FALSE] = &&CASE_OP_PUSH_FALSE,
        [OP_PUSH_INT8] = &&CASE_OP_PUSH_INT8,
        [OP_PUSH_INT] = &&CASE_OP_PUSH_INT,
        [OP_PUSH_SYM] = &&CASE_OP_PUSH_SYM,
        [OP_LOAD] = &&CASE_OP_LOAD,
        [OP_STORE] = &&CASE_OP_STORE,
        [OP_CONS] = &&CASE_OP_CONS,
        [OP_CAR] = &&CASE_OP_CAR,
        [OP_CDR] = &&CASE_OP_CDR,
        [OP_ADD] = &&CASE_OP_ADD,
        [OP_SUB] = &&CASE_OP_SUB,
        [OP_MUL] = &&CASE_OP_MUL,
        [OP_DIV] = &&CASE_OP_DIV,
        [OP_MOD] = &&CASE_OP_MOD,
        [OP_EQ] = &&CASE_OP_EQ,
        [OP_NE] = &&CASE_OP_NE,
        [OP_LT] = &&CASE_OP_LT,
        [OP_LE] = &&CASE_OP_LE,
        [OP_IS_NIL] = &&CASE_OP_IS_NIL,
        [OP_IS_PAIR] = &&CASE_OP_IS_PAIR,
        [OP_IS_INT] = &&CASE_OP_IS_INT,
        [OP_IS_STRING] = &&CASE_OP_IS_STRING,
        [OP_IS_BYTES] = &&CASE_OP_IS_BYTES,
        [OP_IS_PID] = &&CASE_OP_IS_PID,
        [OP_JUMP] = &&CASE_OP_JUMP,
        [OP_JUMP_IF_FALSE] = &&CASE_OP_JUMP_IF_FALSE,
        [OP_POP] = &&CASE_OP_POP,
        [OP_DUP] = &&CASE_OP_DUP,
        [OP_PUSH_STRING] = &&CASE_OP_PUSH_STRING,
        [OP_PUSH_FLOAT] = &&CASE_OP_PUSH_FLOAT,
        [OP_CLOSURE] = &&CASE_OP_CLOSURE,
        [OP_CALL] = &&CASE_OP_CALL,
        [OP_TAIL_CALL] = &&CASE_OP_TAIL_CALL,
        [OP_RET] = &&CASE_OP_RET,
        [OP_ENTER] = &&CASE_OP_ENTER,
        /* Reserved numbers: the actor primitives used to live here and now go
         * through OP_BUILTIN. The entries keep the table total over
         * [0, OP_COUNT) — see the reserved arms below. */
        [OP_RESERVED_SPAWN] = &&CASE_OP_RESERVED_SPAWN,
        [OP_RESERVED_SPAWN_MAIN] = &&CASE_OP_RESERVED_SPAWN_MAIN,
        [OP_RESERVED_SPAWN_CLOS] = &&CASE_OP_RESERVED_SPAWN_CLOS,
        [OP_RESERVED_SEND] = &&CASE_OP_RESERVED_SEND,
        [OP_RESERVED_RECV] = &&CASE_OP_RESERVED_RECV,
        [OP_RESERVED_RECV_PEEK] = &&CASE_OP_RESERVED_RECV_PEEK,
        [OP_RESERVED_RECV_COMMIT] = &&CASE_OP_RESERVED_RECV_COMMIT,
        [OP_SELF] = &&CASE_OP_SELF,
        [OP_RESERVED_MONITOR] = &&CASE_OP_RESERVED_MONITOR,
        [OP_RESERVED_RECV_AFTER] = &&CASE_OP_RESERVED_RECV_AFTER,
        [OP_HALT] = &&CASE_OP_HALT,
        [OP_CCALL_NAME] = &&CASE_OP_CCALL_NAME,
        [OP_BUILTIN] = &&CASE_OP_BUILTIN,
    };

    /* The block below is the dispatch skeleton described in the comment above
     * it: handler labels are jump targets, and each tail is hand-spelled, so
     * it stays out of clang-format's hands. */
    /* clang-format off */
    if (reductions <= 0) {
        p->pc = pc;
        p->sp = sp; /* budget-exhausted exit, spelled like TICK_FETCH's */
        return 0;
    }
    assert(p->gc_gate == 0);
    op = p->code[pc++];

    /* Enter the dispatch loop: jump to the handler of the opcode just
     * fetched, exactly as every handler tail below does. */
    if (op >= OP_COUNT)
        goto CASE_OP_UNKNOWN;
    goto *dispatch_table[op];

    /* ---- stack constants ---- */
    CASE_OP_PUSH_NIL:
    SP_PUSH(val_nil());
    TICK_FETCH();
    if (op >= OP_COUNT)
        goto CASE_OP_UNKNOWN;
    goto *dispatch_table[op];
    CASE_OP_PUSH_TRUE:
    SP_PUSH(val_true());
    TICK_FETCH();
    if (op >= OP_COUNT)
        goto CASE_OP_UNKNOWN;
    goto *dispatch_table[op];
    CASE_OP_PUSH_FALSE:
    SP_PUSH(val_false());
    TICK_FETCH();
    if (op >= OP_COUNT)
        goto CASE_OP_UNKNOWN;
    goto *dispatch_table[op];

    CASE_OP_PUSH_INT8: {
        int8_t i8 = (int8_t)p->code[pc++];
        SP_PUSH(val_int(i8));
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }
    CASE_OP_PUSH_INT: {
        int64_t i64;
        memcpy(&i64, &p->code[pc], 8);
        pc += 8;
        SP_PUSH(val_int(i64));
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }
    CASE_OP_PUSH_SYM: {
        int32_t idx;
        memcpy(&idx, &p->code[pc], 4);
        pc += 4;
        SP_PUSH(val_symbol((uint32_t)idx));
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }

    /* ---- local variables ---- */
    CASE_OP_LOAD: {
        int32_t off;
        memcpy(&off, &p->code[pc], 4);
        pc += 4;
        SP_PUSH(proc_stack(p)[p->fp + off]);
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }
    CASE_OP_STORE: {
        int32_t off;
        memcpy(&off, &p->code[pc], 4);
        pc += 4;
        proc_stack(p)[p->fp + off] = SP_POP();
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }

    /* ---- pair ---- */
    CASE_OP_CONS: {
        /* Allocate first, then read the operands straight from the stack:
         * a collection inside proc_heap_alloc forwards stack slots in
         * place, so reading afterwards sees the moved car/cdr. Popping
         * them into C locals first would leave those copies dangling
         * after a move — and the two slots must stay rooted across the
         * call, which is what publishing sp before it is for: the
         * collector scans [p->sp, 0), so both operands are in range. */
        SP_PUBLISH();
        HeapPair *hp = (HeapPair *)proc_heap_alloc(p, sizeof(HeapPair));
        hp->hdr.type = HEAP_PAIR;
        hp->hdr.flags = 0;
        hp->car = SP_PEEK(1); /* car pushed first → below cdr */
        hp->cdr = SP_PEEK(0);
        sp += 2; /* drop car + cdr */
        SP_PUSH(((Val)TAG_PAIR << 48) | (uint64_t)(uintptr_t)hp);
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }
    CASE_OP_CAR: {
        Val v = SP_POP();
        if (val_is_nil(v)) {
            SP_PUSH(val_nil());
        } else if (val_is_pair(v)) {
            SP_PUSH(val_get_car(v));
        } else {
            /* car of a non-pair (and non-nil): runtime type error instead of
             * dereferencing a garbage pointer (previously a segfault). */
            fprintf(stderr, "error: car: expected pair or nil, got tag=0x%04llx (raw=0x%llx)\n",
                    (unsigned long long)(v >> 48), (unsigned long long)v);
            int carerr = vm_intern_symbol(vm, "cartype");
            p->pc = pc;
            SP_PUBLISH(); /* proc_die reserves room on this heap */
            proc_die(vm, p, val_symbol((uint32_t)carerr));
            return -1;
        }
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }
    CASE_OP_CDR: {
        Val v = SP_POP();
        if (val_is_nil(v)) {
            SP_PUSH(val_nil());
        } else if (val_is_pair(v)) {
            SP_PUSH(val_get_cdr(v));
        } else {
            fprintf(stderr, "error: cdr: expected pair or nil, got tag=0x%04llx (raw=0x%llx)\n",
                    (unsigned long long)(v >> 48), (unsigned long long)v);
            int cdreerr = vm_intern_symbol(vm, "cdrtype");
            p->pc = pc;
            SP_PUBLISH(); /* proc_die reserves room on this heap */
            proc_die(vm, p, val_symbol((uint32_t)cdreerr));
            return -1;
        }
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }

    /* ---- arithmetic ---- */
    /* Mixed int/float: if EITHER operand is a float, the operation is done in
     * double precision and the result is a float (never narrowed back to int),
     * so `1 + 1.5` → 2.5 and `3.0 / 2` → 1.5. Pure int stays int (3/2 == 1). */
    CASE_OP_ADD: {
        Val b = SP_POP();
        Val a = SP_POP();
        /* Only int+int stays int. Any non-int operand (pair/string/bool/nil —
         * whose NaN-box payload is NOT a small integer) degrades to 0.0 via
         * val_to_double, mirroring the golden reference (golden.py _binop /
         * _float_of). The old else-branch called val_get_int on such values
         * and read a heap pointer's low 48 bits as an int, producing
         * nondeterministic garbage (kernfuzz anchor-crash). */
        if (val_is_int(a) && val_is_int(b))
            SP_PUSH(val_int(val_get_int(a) + val_get_int(b)));
        else
            SP_PUSH(val_from_double(val_to_double(a) + val_to_double(b)));
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }
    CASE_OP_SUB: {
        Val b = SP_POP();
        Val a = SP_POP();
        /* int-int stays int; else float with non-numeric degrading to 0.0
         * (see OP_ADD note — matches golden reference). */
        if (val_is_int(a) && val_is_int(b))
            SP_PUSH(val_int(val_get_int(a) - val_get_int(b)));
        else
            SP_PUSH(val_from_double(val_to_double(a) - val_to_double(b)));
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }
    CASE_OP_MUL: {
        Val b = SP_POP();
        Val a = SP_POP();
        /* int-int stays int; else float with non-numeric degrading to 0.0
         * (see OP_ADD note — matches golden reference). */
        if (val_is_int(a) && val_is_int(b))
            SP_PUSH(val_int(val_get_int(a) * val_get_int(b)));
        else
            SP_PUSH(val_from_double(val_to_double(a) * val_to_double(b)));
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }
    CASE_OP_DIV: {
        Val b = SP_POP();
        Val a = SP_POP();
        if (val_is_int(a) && val_is_int(b)) {
            if (val_get_int(b) == 0) {
                /* Division by zero: kill only this process, deliver DOWN with
                 * reason 'divzero (process isolation — other procs continue). */
                int divzero = vm_intern_symbol(vm, "divzero");
                p->pc = pc;
                SP_PUBLISH(); /* proc_die reserves room on this heap */
                proc_die(vm, p, val_symbol((uint32_t)divzero));
                return -1;
            }
            SP_PUSH(val_int(val_get_int(a) / val_get_int(b)));
            TICK_FETCH();
            if (op >= OP_COUNT)
                goto CASE_OP_UNKNOWN;
            goto *dispatch_table[op];
        }
        /* Mixed/non-int (e.g. a receive-bound string): float path, where
         * val_to_double degrades non-numerics to 0.0 (golden.py _binop —
         * only the both-int case stays on the integer path, so a dynamic
         * operand can no longer reach val_get_int and read its NaN-box
         * payload as an int; issue #158). Division by zero yields ±inf. */
        SP_PUSH(val_from_double(val_to_double(a) / val_to_double(b)));
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }
    CASE_OP_MOD: {
        Val b = SP_POP();
        Val a = SP_POP();
        /* % is int-only (golden.py _binop raises "% is int-only"): a
         * non-int operand — typecheck-rejected statically but reachable
         * dynamically through a receive-bound value — used to hit
         * val_get_int and read its NaN-box payload as an int, producing
         * garbage or a spurious 'divzero (issue #158). Die instead, same
         * process isolation as 'divzero. */
        if (!val_is_int(a) || !val_is_int(b)) {
            fprintf(stderr, "error: mod: expected int operands, got tag=0x%04llx / tag=0x%04llx\n",
                    (unsigned long long)(a >> 48), (unsigned long long)(b >> 48));
            int arithtype = vm_intern_symbol(vm, "arithtype");
            p->pc = pc;
            SP_PUBLISH(); /* proc_die reserves room on this heap */
            proc_die(vm, p, val_symbol((uint32_t)arithtype));
            return -1;
        }
        if (val_get_int(b) == 0) {
            int divzero = vm_intern_symbol(vm, "divzero");
            p->pc = pc;
            SP_PUBLISH(); /* proc_die reserves room on this heap */
            proc_die(vm, p, val_symbol((uint32_t)divzero));
            return -1;
        }
        SP_PUSH(val_int(val_get_int(a) % val_get_int(b)));
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }

    /* ---- comparison ---- */
    /* The double-precision path is taken only when ONE operand is a float
     * and the other is numeric too (int/float): 3 == 3.0 → true, 2.5 < 3 →
     * true. int/int stays on the integer path of the fallback branch (see
     * val_is_num). The double path must NOT be reached for a non-numeric
     * operand — val_to_double maps every non-numeric (string/symbol/pair/...)
     * to 0.0, which would make `"str" == 0.0` and `"str" <= 0.0` true (bug: a
     * value bound by receive is dynamic, so this reaches the VM even though
     * typecheck rejects literal mixes — strict numeric tower, issue #92).
     * Mixed/non-numeric pairs fall back to type-strict comparison: EQ/NE
     * through val_equal (content/value/identity), LT/LE simply false. */
    CASE_OP_EQ: {
        Val b = SP_POP();
        Val a = SP_POP();
        int eq;
        if (cmp_numeric_path(a, b))
            eq = val_to_double(a) == val_to_double(b);
        else
            eq = val_equal(a, b);
        SP_PUSH(eq ? val_true() : val_false());
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }
    CASE_OP_NE: {
        Val b = SP_POP();
        Val a = SP_POP();
        int ne;
        if (cmp_numeric_path(a, b))
            ne = val_to_double(a) != val_to_double(b);
        else
            ne = !val_equal(a, b);
        SP_PUSH(ne ? val_true() : val_false());
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }
    CASE_OP_LT: {
        Val b = SP_POP();
        Val a = SP_POP();
        int cmp;
        if (cmp_numeric_path(a, b))
            cmp = val_to_double(a) < val_to_double(b);
        else
            cmp = val_is_int(a) && val_is_int(b) && (val_get_int(a) < val_get_int(b));
        SP_PUSH(cmp ? val_true() : val_false());
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }
    CASE_OP_LE: {
        Val b = SP_POP();
        Val a = SP_POP();
        int cmp;
        if (cmp_numeric_path(a, b))
            cmp = val_to_double(a) <= val_to_double(b);
        else
            cmp = val_is_int(a) && val_is_int(b) && (val_get_int(a) <= val_get_int(b));
        SP_PUSH(cmp ? val_true() : val_false());
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }

    /* ---- type tests ---- */
    CASE_OP_IS_NIL: {
        Val v = SP_POP();
        SP_PUSH(val_is_nil(v) ? val_true() : val_false());
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }
    CASE_OP_IS_PAIR: {
        Val v = SP_POP();
        SP_PUSH(val_is_pair(v) ? val_true() : val_false());
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }
    CASE_OP_IS_INT: {
        Val v = SP_POP();
        SP_PUSH(val_is_int(v) ? val_true() : val_false());
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }
    CASE_OP_IS_STRING: {
        Val v = SP_POP();
        SP_PUSH(val_is_string(v) ? val_true() : val_false());
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }
    CASE_OP_IS_BYTES: {
        Val v = SP_POP();
        SP_PUSH(val_is_bytes(v) ? val_true() : val_false());
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }
    CASE_OP_IS_PID: {
        Val v = SP_POP();
        SP_PUSH(val_is_pid(v) ? val_true() : val_false());
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }

    /* ---- control flow ---- */
    CASE_OP_JUMP: {
        int32_t addr;
        memcpy(&addr, &p->code[pc], 4);
        pc = addr;
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }
    CASE_OP_JUMP_IF_FALSE: {
        int32_t addr;
        memcpy(&addr, &p->code[pc], 4);
        pc += 4;
        Val v = SP_POP();
        if (val_is_nil(v) || v == val_false())
            pc = addr;
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }
    CASE_OP_POP:
    SP_POP();
    TICK_FETCH();
    if (op >= OP_COUNT)
        goto CASE_OP_UNKNOWN;
    goto *dispatch_table[op];
    CASE_OP_DUP: {
        /* Read then push: SP_PUSH reads sp to compute the slot it writes, so
         * passing SP_PEEK(0) directly would leave the two accesses to sp
         * unsequenced (UB, and clang is free to hand back the new slot). */
        Val v = SP_PEEK(0);
        SP_PUSH(v);
    }
    TICK_FETCH();
    if (op >= OP_COUNT)
        goto CASE_OP_UNKNOWN;
    goto *dispatch_table[op];

    CASE_OP_PUSH_STRING: {
        int32_t len;
        memcpy(&len, &p->code[pc], 4);
        pc += 4;
        SP_PUBLISH(); /* proc_heap_alloc may collect: sp is the root scan's lower bound */
        HeapString *s = (HeapString *)proc_heap_alloc(p, sizeof(HeapString) + len + 1);
        s->hdr.type = HEAP_STRING;
        s->hdr.flags = 0;
        s->len = len;
        memcpy(s->data, &p->code[pc], len);
        s->data[len] = '\0';
        pc += len;
        Val v = ((Val)TAG_STRING << 48) | (uint64_t)(uintptr_t)s;
        SP_PUSH(v);
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }

    CASE_OP_PUSH_FLOAT: {
        /* Float literals travel as decimal strings through the compiler
         * (the bootstrap language has no float values); the VM parses them
         * with strtod at load/runtime. */
        int32_t len;
        memcpy(&len, &p->code[pc], 4);
        pc += 4;
        char buf[64];
        int32_t n = len < (int32_t)sizeof(buf) - 1 ? len : (int32_t)sizeof(buf) - 1;
        memcpy(buf, &p->code[pc], n);
        buf[n] = '\0';
        pc += len;
        SP_PUSH(val_float(strtod(buf, NULL)));
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }

    /* ---- functions ---- */
    CASE_OP_CLOSURE: {
        int32_t fn_id, nfree;
        memcpy(&fn_id, &p->code[pc], 4);
        pc += 4;
        memcpy(&nfree, &p->code[pc], 4);
        pc += 4;
        if (nfree == 0) {
            /* No free vars — encode fn_id directly, no heap alloc */
            Val v = ((Val)TAG_CLOS_ID << 48) | (uint64_t)(uint32_t)fn_id;
            SP_PUSH(v);
            TICK_FETCH();
            if (op >= OP_COUNT)
                goto CASE_OP_UNKNOWN;
            goto *dispatch_table[op];
        }
        /* proc_heap_alloc cannot fail (arena exhaustion is fatal), so no
         * OOM path is needed here. */
        SP_PUBLISH(); /* proc_heap_alloc may collect: sp is the root scan's lower bound */
        HeapClosure *clos =
            (HeapClosure *)proc_heap_alloc(p, sizeof(HeapClosure) + nfree * (int)sizeof(Val));
        clos->hdr.type = HEAP_CLOS;
        clos->entry = fn_id;
        clos->nfree = nfree;
        for (int i = 0; i < nfree; i++) {
            int32_t off;
            memcpy(&off, &p->code[pc], 4);
            pc += 4;
            clos->free[i] = proc_stack(p)[p->fp + off];
        }
        Val v = ((Val)TAG_CLOS << 48) | (uint64_t)(uintptr_t)clos;
        SP_PUSH(v);
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }

    CASE_OP_CALL: {
        int32_t nargs;
        memcpy(&nargs, &p->code[pc], 4);
        pc += 4;
        /* Calling convention (compile_general_call): the caller pushes
         * the closure first, then arg0..argN-1, so on entry the closure
         * sits at sp+nargs and arg_j at sp+nargs-1-j. */
        Val closure_val = SP_PEEK(nargs);
        if ((closure_val >> 48) != TAG_CLOS && (closure_val >> 48) != TAG_CLOS_ID) {
            /* Find which function contains this pc */
            int fn_id = -1;
            for (int i = 0; i < p->fn_count; i++) {
                int next_start = (i + 1 < p->fn_count) ? p->fn_table[i + 1] : INT_MAX;
                if (pc - 4 >= p->fn_table[i] && pc - 4 < next_start) {
                    fn_id = i;
                    break;
                }
            }
            fprintf(stderr,
                    "error: cannot call non-function value (tag=0x%04llx, raw=0x%llx, pc=%d, "
                    "fn=%d, nargs=%d)\n",
                    (unsigned long long)(closure_val >> 48), (unsigned long long)closure_val,
                    pc - 4, fn_id, nargs);
            int notafn = vm_intern_symbol(vm, "notafunction");
            p->pc = pc;
            SP_PUBLISH(); /* proc_die reserves room on this heap */
            proc_die(vm, p, val_symbol((uint32_t)notafn));
            return -1;
        }

        int nfree = 0;
        if ((closure_val >> 48) == TAG_CLOS)
            nfree = val_as_clos(closure_val)->nfree;

        /* Frame geometry: once the closure+args are consumed the caller's
         * stack top is sp+nargs+1, and fp sits nfree+nargs below it. The
         * new frame lives in [fp-4 .. sp+nargs]: it grows down into slots
         * the operands did not occupy by nfree+3 (see the frame-layout
         * note above vm_walk_stack). */
        int caller_sp = sp + nargs + 1;
        int fp = caller_sp - nfree - nargs;
        /* Reserve that whole span at once (the old push-by-push code grew
         * stack as it went); the range test is proc_push's collision check
         * for a whole frame instead of one slot. Publishing first is what
         * makes the reserve see the real top: it grows the arena (only
         * while the heap is empty) by memmove'ing the stack to the new high
         * end, and a stale p->sp would leave the operands just pushed below
         * sp behind in the middle of the buffer. */
        SP_PUBLISH();
        proc_stack_reserve(p, fp - 4);

        /* Rearrange the operands in place — no C buffer. They occupy, low
         * to high index, [argN-1 .. arg0, closure]; reversing yields
         * [closure, arg0 .. argN-1], then shifting the block down by nfree
         * opens the slot above the args where the free vars go. No GC can
         * run mid-arrangement: this handler never calls proc_heap_alloc,
         * and proc_stack_reserve grows the arena only while the heap is
         * still empty, so no live heap Val is invalidated (#136). */
        Val *st = proc_stack(p);
        stack_reverse(st, sp, nargs + 1);
        memmove(st + sp - nfree, st + sp, (size_t)(nargs + 1) * sizeof(Val));
        if (nfree > 0) {
            HeapClosure *clos = val_as_clos(closure_val);
            for (int i = 0; i < nfree; i++)
                st[fp + nargs + i] = clos->free[i];
        }
        /* The shifted block put the closure at fp-1; write the return
         * context below it. */
        st[fp - 2] = val_int(pc);        /* ret_pc */
        st[fp - 3] = val_int(p->fp);     /* old_fp */
        st[fp - 4] = val_int(caller_sp); /* caller_sp */

        p->fp = fp;
        sp = fp - 4; /* the callee's frame is live: enter it with an empty body */
        if ((closure_val >> 48) == TAG_CLOS_ID)
            pc = p->fn_table[(int)(closure_val & 0xFFFFFFFFFFFFULL)];
        else
            pc = p->fn_table[val_as_clos(closure_val)->entry];
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }

    CASE_OP_TAIL_CALL: {
        int32_t nargs;
        memcpy(&nargs, &p->code[pc], 4);
        pc += 4;
        Val closure_val = SP_PEEK(nargs);
        if ((closure_val >> 48) != TAG_CLOS && (closure_val >> 48) != TAG_CLOS_ID) {
            fprintf(stderr, "error: cannot call non-function value\n");
            int notafn = vm_intern_symbol(vm, "notafunction");
            p->pc = pc;
            SP_PUBLISH(); /* proc_die reserves room on this heap */
            proc_die(vm, p, val_symbol((uint32_t)notafn));
            return -1;
        }
        int nfree = 0;
        if ((closure_val >> 48) == TAG_CLOS)
            nfree = val_as_clos(closure_val)->nfree;

        /* A tail call replaces this frame's body but keeps its caller, so
         * the current frame's return context is reused verbatim. Read it
         * out before anything below is overwritten. */
        Val *st = proc_stack(p);
        int caller_sp = (int)val_get_int(st[p->fp - 4]);
        int old_fp = (int)val_get_int(st[p->fp - 3]);
        int ret_pc = (int)val_get_int(st[p->fp - 2]);
        int fp = caller_sp - nfree - nargs;

        if (closure_val == st[p->fp - 1] && fp == p->fp) {
            /* Self-recursive fast path: the callee is this very frame's
             * closure (same Val — TAG_CLOS and TAG_CLOS_ID alike, no deep
             * compare) and the geometry is unchanged, so free vars, frame
             * header and frame size are all reused as-is. Only the new
             * args move to fp+0 and sp resets to the empty body. */
            stack_reverse(st, sp, nargs);
            memmove(st + p->fp, st + sp, (size_t)nargs * sizeof(Val));
            sp = p->fp - 4; /* tail-called frame: same header, empty body */
        } else {
            /* General in-place rebuild: discard this frame's locals+body
             * (its 4 header slots are re-written at the new fp), reverse
             * the operands into the body, splice in the new closure's free
             * vars. Same no-GC discipline as OP_CALL: proc_stack_reserve
             * can only move the arena while the heap is empty (#136), and
             * this handler never calls proc_heap_alloc. Publishing first is
             * what makes that (possible) move see the real top — the
             * operands just pushed below sp — as it shifts the stack. */
            SP_PUBLISH();
            proc_stack_reserve(p, fp - 4);
            /* Reserve may have moved the arena (it grows only while the
             * heap is still empty, #136), so the base pointer read above
             * is stale — re-take it before touching the stack. */
            st = proc_stack(p);
            stack_reverse(st, sp, nargs);
            memmove(st + fp, st + sp, (size_t)nargs * sizeof(Val));
            st[fp - 1] = closure_val; /* closure */
            if (nfree > 0) {
                HeapClosure *clos = val_as_clos(closure_val);
                for (int i = 0; i < nfree; i++)
                    st[fp + nargs + i] = clos->free[i];
            }
            st[fp - 2] = val_int(ret_pc);
            st[fp - 3] = val_int(old_fp);
            st[fp - 4] = val_int(caller_sp);
            /* Frame geometry must match the layout documented above
             * vm_walk_stack (args at fp+0.., closure at fp-1). */
            assert(st[fp - 1] == closure_val);
            p->fp = fp;
            sp = fp - 4; /* tail-called frame is live: empty body */
        }

        if ((closure_val >> 48) == TAG_CLOS_ID)
            pc = p->fn_table[(int)(closure_val & 0xFFFFFFFFFFFFULL)];
        else
            pc = p->fn_table[val_as_clos(closure_val)->entry];
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }

    CASE_OP_RET: {
        Val ret_val = SP_POP();
        int caller_sp = (int)val_get_int(proc_stack(p)[p->fp - 4]);
        int old_fp = (int)val_get_int(proc_stack(p)[p->fp - 3]);
        int ret_addr = (int)val_get_int(proc_stack(p)[p->fp - 2]);

        sp = caller_sp; /* release the callee's frame; ret_val is a C local */
        p->fp = old_fp;
        if (ret_addr < 0) {
            /* root frame: main() returned, the proc exits normally. The
             * local ret_val is not on the stack here, and proc_die reserves
             * room on this heap from p->sp, so publish. */
            p->pc = pc;
            SP_PUBLISH();
            proc_die(vm, p, val_nil());
            return -1;
        }
        pc = ret_addr;
        SP_PUSH(ret_val);
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }

    CASE_OP_ENTER: {
        /* Reserve stack space for local variables up front — the frame
         * region codegen reports as `nslots`, which is what the rest of the
         * function reaches with LOAD/STORE — instead of growing the stack
         * one checked slot at a time as the body runs. That is the whole
         * reason a frame can be entered without touching the collision
         * check per slot (see the SP_* macros above); the operands that
         * pile up *below* the frame are the boundary check's business, so
         * the reservation stops here rather than at some "deepest
         * temporary" guess. The one slot of slack over the reserved range
         * is just slack. */
        int32_t nslots;
        memcpy(&nslots, &p->code[pc], 4);
        pc += 4;
        SP_PUBLISH();
        proc_stack_reserve(p, sp - nslots - 1);
        for (int i = 0; i < nslots; i++)
            SP_PUSH(val_nil()); /* nil-filled frame: GC sees safe values */
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }

    /* ---- actor primitives ---- */
    /* Only `self` remains an opcode; the other nine actor primitives now go
     * through OP_BUILTIN below. */
    CASE_OP_SELF:
    SP_PUSH(val_pid((uint32_t)p->pid));
    TICK_FETCH();
    if (op >= OP_COUNT)
        goto CASE_OP_UNKNOWN;
    goto *dispatch_table[op];

    /* Reserved opcode numbers: the nine actor primitives that moved behind
     * OP_BUILTIN. Nothing emits them any more — they exist only so the
     * surviving opcode numbers stay put — so a hit here means a corrupt or
     * pre-v3 image, and they share the unknown-opcode report with any
     * out-of-range op. */
    CASE_OP_RESERVED_SPAWN:
    CASE_OP_RESERVED_SPAWN_MAIN:
    CASE_OP_RESERVED_SPAWN_CLOS:
    CASE_OP_RESERVED_SEND:
    CASE_OP_RESERVED_RECV:
    CASE_OP_RESERVED_RECV_PEEK:
    CASE_OP_RESERVED_RECV_COMMIT:
    CASE_OP_RESERVED_MONITOR:
    CASE_OP_RESERVED_RECV_AFTER:
        goto CASE_OP_UNKNOWN;

    /* OP_BUILTIN idx: one of the cold-path actor primitives (spawn / send /
     * receive / monitor …), dispatched through builtin_table[] in
     * src/builtin.c. The builtin owns p->sp and p->pc for the call: it reads
     * its own operands from p->code past the idx and pushes its result
     * through p->sp, so the loop-local sp/pc stay untouched until it
     * returns. This is the same "publish, call out, reload" boundary as
     * OP_CCALL_NAME. */
    CASE_OP_BUILTIN: {
        int pc_op_start = pc - 1; /* the OP_BUILTIN byte: rewind point for a block */
        uint8_t bidx = p->code[pc++];
        if (bidx >= BUILTIN_COUNT) {
            fprintf(stderr, "vm_run_proc: unknown builtin %d at pc=%d\n", bidx, pc_op_start);
            int badop = vm_intern_symbol(vm, "badopcode");
            SP_PUBLISH(); /* proc_die reserves room on this heap from p->sp */
            p->pc = pc;
            proc_die(vm, p, val_symbol((uint32_t)badop));
            return -1;
        }
        SP_PUBLISH(); /* the builtin reads the real stack top */
        p->pc = pc;   /* …and its operands from here; it advances p->pc itself */
        if (builtin_table[bidx](vm, p) == B_SUSPEND) {
            /* Blocked: rewind to this instruction so the scheduler re-runs
             * the whole thing (operands and all) on wake. p->sp/p->pc are
             * the builtin's; the loop locals are not consulted again. */
            p->pc = pc_op_start;
            return -1;
        }
        /* Cold path done: re-take what the builtin left behind, exactly like
         * OP_CCALL_NAME. */
        pc = p->pc;
        sp = p->sp;
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }

    /* ---- built-in ---- */
    CASE_OP_HALT:
    vm->eval_result = SP_PEEK(0);
    p->pc = pc;
    SP_PUBLISH(); /* proc_die reserves room on this heap from p->sp */
    proc_die(vm, p, val_nil());
    return -1;

    CASE_OP_CCALL_NAME: {
        int pc_start = pc - 1; /* save for rewind on yield */
        int sym_idx;
        memcpy(&sym_idx, p->code + pc, 4);
        pc += 4;
        uint8_t nc = p->code[pc++];
        if (sym_idx < 0 || sym_idx >= vm->sym_count) {
            for (int i = 0; i < nc; i++)
                SP_POP();
            SP_PUSH(val_nil());
            TICK_FETCH();
            if (op >= OP_COUNT)
                goto CASE_OP_UNKNOWN;
            goto *dispatch_table[op];
        }
        if (nc > 64) {
            for (int i = 0; i < nc; i++)
                SP_POP();
            SP_PUSH(val_nil());
            TICK_FETCH();
            if (op >= OP_COUNT)
                goto CASE_OP_UNKNOWN;
            goto *dispatch_table[op];
        }
        const char *name = vm->symbols[sym_idx];
        /* From here on this handler runs arbitrary C code — the auto-load
         * below (dlopen + module registration, which allocates through
         * tls_current_proc) and then the callback. The operands are still on
         * the stack while that happens, so it must see the real top: publish
         * before the first of them. (The loads above cannot allocate, and the
         * early exits below only pop, so neither needs this.) */
        SP_PUBLISH();
        int cfidx = vm_find_cfunc(vm, name);
        if (cfidx < 0) {
            /* Erlang-style auto-load: if name contains a dot, try dlopen
             * lib/<module>.<ext> and retry the lookup. macOS modules are
             * .dylib, Linux are .so. */
            const char *dot = strchr(name, '.');
            if (dot) {
                int mod_len = (int)(dot - name);
#ifdef __APPLE__
                const char *ext = "dylib";
#else
                        const char *ext = "so";
#endif
                char mod_path[256];
#ifdef TA_MOD_TAG
                int n = snprintf(mod_path, sizeof(mod_path), "lib/%.*s_%s.%s", mod_len, name,
                                 TA_MOD_TAG_STR(TA_MOD_TAG), ext);
#else
                        int n =
                            snprintf(mod_path, sizeof(mod_path), "lib/%.*s.%s", mod_len, name, ext);
#endif
                if (n > 0 && n < (int)sizeof(mod_path)) {
                    void *handle = dlopen(mod_path, RTLD_NOW | RTLD_GLOBAL);
                    if (handle) {
                        void (*reg)(VM *) = (void (*)(VM *))dlsym(handle, "vm_load_self");
                        if (reg)
                            reg(vm);
                        cfidx = vm_find_cfunc(vm, name);
                    }
                }
            }
            if (cfidx < 0) {
                for (int i = 0; i < nc; i++)
                    SP_POP();
                SP_PUSH(val_nil());
                TICK_FETCH();
                if (op >= OP_COUNT)
                    goto CASE_OP_UNKNOWN;
                goto *dispatch_table[op];
            }
        }
        Val args[64];
        for (int i = nc - 1; i >= 0; i--)
            args[i] = SP_POP();
        /* The operands are off the stack now (they live in args, and the gate
         * is about to close, so no collection can move what they point at).
         * Publish the top the callback must operate on: it reaches this proc
         * through tls_current_proc and pushes/pops with the C API, which work
         * on p->sp — with the pre-pop value still there, the reload below
         * would hand back the operands we just consumed. */
        SP_PUBLISH();
        tls_current_proc = p;
        p->yield_requested = 0;
        p->die_requested = 0;
        /* The C module may allocate on p's heap while holding Vals it got
         * from args (or its own root-free state) in C locals, so no
         * collection may run inside the callback: close the gate. */
        proc_gc_enter(p);
        p->in_ccall = 1;
        Val result = vm->cfuncs[cfidx].fn(vm, args, nc);
        /* The callback owns p->sp while it runs (proc_push / the C API), so
         * the local value is a guess now: reload. This also makes p->sp
         * current for everything below — the die path reserves room through
         * proc_die, and proc_chunk_converge sizes the free heap from it —
         * while the callback's own pushes/pops are accounted for
         * (proc_push published them as it went). */
        sp = p->sp;
        if (p->die_requested) {
            /* Builtin raised a runtime error (vm_die): kill this proc with
             * the reason symbol, exactly like an opcode type error. */
            p->die_requested = 0;
            Val reason = p->die_reason;
            p->in_ccall = 0;
            proc_gc_leave(p);
            p->pc = pc;
            /* No publish needed: p->sp was reloaded from the callback just
             * above and nothing has moved since. */
            proc_die(vm, p, reason);
            return -1;
        }
        if (p->yield_requested) {
            /* The callback will be re-run from scratch (pc_start), so its
             * partial chunk arena is garbage: drop it and clear in_ccall,
             * otherwise the stale total silently inflates the next
             * convergence and allocations between resume and the re-entered
             * OP_CCALL would route into the stale arena. */
            p->in_ccall = 0;
            proc_chunk_reset(p);
            /* Re-root args before reopening the gate: the callback left
             * them in C locals, but the stack is the root set. */
            for (int i = 0; i < nc; i++)
                SP_PUSH(args[i]);
            SP_PUBLISH(); /* re-rooted: the reopen drains, scanning [p->sp, 0) */
            proc_gc_reopen(p);
            atomic_store(&p->state, PROC_WAIT_IO);
            p->pc = pc_start;
            return -1;
        }
        p->in_ccall = 0;
        /* Converge the callback's chunk arena into the heap before the
         * result is rooted (issue #160): chunk addresses are stable, and
         * convergence needs the result only as a Val, not a rooted one. */
        proc_chunk_converge(p, &result);
        SP_PUSH(result);
        /* result is rooted now; a callback that allocated heavily left a
         * pending request the gate suppressed, so honour it here rather
         * than let a cfunc loop grow the heap unbounded. The honouring is a
         * collection (reopen drains), hence the publish: result has to be
         * in p->sp before the collector looks. */
        SP_PUBLISH();
        proc_gc_reopen(p);
        TICK_FETCH();
        if (op >= OP_COUNT)
            goto CASE_OP_UNKNOWN;
        goto *dispatch_table[op];
    }
    /* Reached from the dispatch entry and from any handler tail's bounds
     * check: an opcode >= OP_COUNT has no table entry, so it is reported here
     * rather than dereferenced. */
CASE_OP_UNKNOWN:
    /* The stack goes to the collector on the way out (proc_die reserves room
     * on this heap, and a dying proc's DOWN messages do the same), and many
     * handlers reach here right after a push — one whose tail's bounds check
     * failed. Write the top back once, for all of them. */
    SP_PUBLISH();
    return vm_unknown_opcode(vm, p, op, pc);
    /* clang-format on */
}
