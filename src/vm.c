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
 * Opcode dispatch (perf series 4/6, spec §D)
 *
 * Every handler body below has exactly one copy; only the dispatch step
 * differs between backends, selected at compile time:
 *
 *   CASE(op)   — open the handler for opcode `op`
 *   NEXT()     — finish a handler and continue with the next instruction
 *   DISPATCH() — transfer control to the handler for the fetched `op`
 *
 * With the computed-goto backend `CASE` names a label reached through a static
 * table of label addresses (`&&CASE_OP_xxx`), and NEXT() ends the handler with
 * its own indirect `goto *dispatch_table[op]`. Because that jump is textually
 * duplicated at every handler, the CPU's branch predictor can learn each
 * opcode's successor separately — the classic Lua interpreter win over the
 * single indirect branch a `switch` compiles to. With the switch backend the
 * same macros expand to `case op:` / `break` / nothing, i.e. the original
 * switch, with identical semantics.
 *
 * That win is compiler- AND code-shape-dependent: the duplicated NEXT() jumps
 * are prime tail-merging / tail-duplication material, so a compiler release —
 * or an unrelated inlining change elsewhere in the VM — can flip the verdict
 * in either direction. History proves it (same source, arm64, interleaved
 * runs):
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
 * Lesson: the verdict swings with code shape, not with the compiler brand.
 * Don't hard-code it per compiler; re-measure with interleaved runs whenever
 * the VM body changes materially, and keep the switch reachable as an escape
 * hatch.
 *
 * Default: computed goto on every compiler that supports labels-as-values
 * (`__GNUC__`, clang included); switch elsewhere. -DUSE_COMPUTED_GOTO=1/=0
 * overrides either default, and `make NO_COMPUTED_GOTO=1` is shorthand for
 * forcing the switch.
 *
 * CASE() takes no colon at the call site: it carries its own, so a handler is
 * introduced as `CASE(OP_ADD) { ... }`, which becomes `CASE_OP_ADD:` (label)
 * or `case OP_ADD:` (switch).
 * ================================================================ */
#if !defined(TA_COMPUTED_GOTO)
#if defined(USE_COMPUTED_GOTO)
#define TA_COMPUTED_GOTO USE_COMPUTED_GOTO
#elif defined(__GNUC__) /* clang included: labels-as-values supported */
#define TA_COMPUTED_GOTO 1
#else
#define TA_COMPUTED_GOTO 0
#endif
#endif

/* Which backend a binary carries, readable from the artifact instead of only
 * from the build log: `strings tavm | grep ta-dispatch`. `used` keeps the
 * literal in .rodata; GNU C only, as this is a build-verification aid rather
 * than VM behaviour. */
#if defined(__GNUC__)
#if TA_COMPUTED_GOTO
static const char ta_dispatch_backend[] __attribute__((used)) = "ta-dispatch: computed-goto";
#else
static const char ta_dispatch_backend[] __attribute__((used)) = "ta-dispatch: switch";
#endif
#endif

/* One instruction's bookkeeping plus the fetch of the following opcode. This
 * is the switch loop's former boundary, now reachable from both backends (the
 * goto NEXT() below and the switch loop tail). Order matches the original:
 * sample with the counter of the instruction just executed, spend one unit of
 * reduction budget, re-assert the GC gate, then fetch. */
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
            return 0;                                                                              \
        }                                                                                          \
        assert(p->gc_gate == 0);                                                                   \
        if (p->heap_ptr > TA_PROC_CHUNK0 &&                                                        \
            p->mem_size - p->heap_ptr + p->sp * 8 < TA_STACK_HEADROOM)                             \
            proc_stack_headroom(p); /* safe point: stack is the whole root set */                  \
        op = p->code[pc++];                                                                        \
    } while (0)

#if TA_COMPUTED_GOTO
#define CASE(op) CASE_##op:
#define DISPATCH()                                                                                 \
    do {                                                                                           \
        if (op >= OP_COUNT)                                                                        \
            goto CASE_OP_UNKNOWN;                                                                  \
        goto *dispatch_table[op];                                                                  \
    } while (0)
#define NEXT()                                                                                     \
    do {                                                                                           \
        TICK_FETCH();                                                                              \
        DISPATCH();                                                                                \
    } while (0)
#else
#define CASE(op) case op:
#define NEXT() break
#define DISPATCH() ((void)0)
#endif

/* Report an opcode with no handler and kill the proc. Two call sites reach it:
 * the switch backend's `default:` arm, and (computed goto) DISPATCH()'s
 * discharge of an out-of-range op. Keeping the body here leaves each arm a
 * single statement; a shared block after the #endif is indented differently by
 * different clang-format versions, so no single spelling of it is green
 * everywhere. `pc` is the pc the opcode was fetched from, hence the reported
 * `pc - 1`; returns -1 to leave vm_run_proc, like the other die paths. */
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
    int prof_on = vm->prof_on;
    uint64_t prof_last = 0;
    if (prof_on)
        prof_last = prof_now_ns();

    /* Instruction counter: exactly one per executed instruction, shared by
     * the reduction budget and the sampling profiler (Lua count-hook style).
     * `op` is a local because a handler (OP_SPAWN_MAIN) still consults the
     * opcode it was dispatched on. */
    int r = 0;
    uint8_t op;

#if TA_COMPUTED_GOTO
    /* Opcode -> handler label. Address-of-label (`&&`) is a GNU C extension;
     * the table is static so it is built once. Every opcode in [0, OP_COUNT)
     * has a handler, so the table is total and DISPATCH() only needs a bounds
     * check; an out-of-range op is routed to CASE_OP_UNKNOWN, which is
     * exactly the switch backend's `default:` arm. */
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
        [OP_SPAWN] = &&CASE_OP_SPAWN,
        [OP_SPAWN_MAIN] = &&CASE_OP_SPAWN_MAIN,
        [OP_SPAWN_CLOS] = &&CASE_OP_SPAWN_CLOS,
        [OP_SEND] = &&CASE_OP_SEND,
        [OP_RECV] = &&CASE_OP_RECV,
        [OP_RECV_PEEK] = &&CASE_OP_RECV_PEEK,
        [OP_RECV_COMMIT] = &&CASE_OP_RECV_COMMIT,
        [OP_SELF] = &&CASE_OP_SELF,
        [OP_MONITOR] = &&CASE_OP_MONITOR,
        [OP_RECV_AFTER] = &&CASE_OP_RECV_AFTER,
        [OP_HALT] = &&CASE_OP_HALT,
        [OP_CCALL_NAME] = &&CASE_OP_CCALL_NAME,
    };
#endif

    /* clang-format off (see the macro block above) */
    if (reductions <= 0) {
        p->pc = pc;
        return 0;
    }
    assert(p->gc_gate == 0);
    op = p->code[pc++];

#if TA_COMPUTED_GOTO
    DISPATCH();
#else
    for (;;) {
        switch (op) {
#endif

    /* ---- stack constants ---- */
    CASE(OP_PUSH_NIL)
    proc_push(p, val_nil());
    NEXT();
    CASE(OP_PUSH_TRUE)
    proc_push(p, val_true());
    NEXT();
    CASE(OP_PUSH_FALSE)
    proc_push(p, val_false());
    NEXT();

    CASE(OP_PUSH_INT8) {
        int8_t i8 = (int8_t)p->code[pc++];
        proc_push(p, val_int(i8));
        NEXT();
    }
    CASE(OP_PUSH_INT) {
        int64_t i64;
        memcpy(&i64, &p->code[pc], 8);
        pc += 8;
        proc_push(p, val_int(i64));
        NEXT();
    }
    CASE(OP_PUSH_SYM) {
        int32_t idx;
        memcpy(&idx, &p->code[pc], 4);
        pc += 4;
        proc_push(p, val_symbol((uint32_t)idx));
        NEXT();
    }

    /* ---- local variables ---- */
    CASE(OP_LOAD) {
        int32_t off;
        memcpy(&off, &p->code[pc], 4);
        pc += 4;
        proc_push(p, proc_stack(p)[p->fp + off]);
        NEXT();
    }
    CASE(OP_STORE) {
        int32_t off;
        memcpy(&off, &p->code[pc], 4);
        pc += 4;
        proc_stack(p)[p->fp + off] = proc_pop(p);
        NEXT();
    }

    /* ---- pair ---- */
    CASE(OP_CONS) {
        /* Allocate first, then read the operands straight from the stack:
         * a collection inside proc_heap_alloc forwards stack slots in
         * place, so reading afterwards sees the moved car/cdr. Popping
         * them into C locals first would leave those copies dangling
         * after a move. */
        HeapPair *hp = (HeapPair *)proc_heap_alloc(p, sizeof(HeapPair));
        hp->hdr.type = HEAP_PAIR;
        hp->hdr.flags = 0;
        hp->car = proc_peek(p, 1); /* car pushed first → below cdr */
        hp->cdr = proc_peek(p, 0);
        p->sp += 2; /* drop car + cdr */
        proc_push(p, ((Val)TAG_PAIR << 48) | (uint64_t)(uintptr_t)hp);
        NEXT();
    }
    CASE(OP_CAR) {
        Val v = proc_pop(p);
        if (val_is_nil(v)) {
            proc_push(p, val_nil());
        } else if (val_is_pair(v)) {
            proc_push(p, val_get_car(v));
        } else {
            /* car of a non-pair (and non-nil): runtime type error instead of
             * dereferencing a garbage pointer (previously a segfault). */
            fprintf(stderr, "error: car: expected pair or nil, got tag=0x%04llx (raw=0x%llx)\n",
                    (unsigned long long)(v >> 48), (unsigned long long)v);
            int carerr = vm_intern_symbol(vm, "cartype");
            p->pc = pc;
            proc_die(vm, p, val_symbol((uint32_t)carerr));
            return -1;
        }
        NEXT();
    }
    CASE(OP_CDR) {
        Val v = proc_pop(p);
        if (val_is_nil(v)) {
            proc_push(p, val_nil());
        } else if (val_is_pair(v)) {
            proc_push(p, val_get_cdr(v));
        } else {
            fprintf(stderr, "error: cdr: expected pair or nil, got tag=0x%04llx (raw=0x%llx)\n",
                    (unsigned long long)(v >> 48), (unsigned long long)v);
            int cdreerr = vm_intern_symbol(vm, "cdrtype");
            p->pc = pc;
            proc_die(vm, p, val_symbol((uint32_t)cdreerr));
            return -1;
        }
        NEXT();
    }

    /* ---- arithmetic ---- */
    /* Mixed int/float: if EITHER operand is a float, the operation is done in
     * double precision and the result is a float (never narrowed back to int),
     * so `1 + 1.5` → 2.5 and `3.0 / 2` → 1.5. Pure int stays int (3/2 == 1). */
    CASE(OP_ADD) {
        Val b = proc_pop(p);
        Val a = proc_pop(p);
        /* Only int+int stays int. Any non-int operand (pair/string/bool/nil —
         * whose NaN-box payload is NOT a small integer) degrades to 0.0 via
         * val_to_double, mirroring the golden reference (golden.py _binop /
         * _float_of). The old else-branch called val_get_int on such values
         * and read a heap pointer's low 48 bits as an int, producing
         * nondeterministic garbage (kernfuzz anchor-crash). */
        if (val_is_int(a) && val_is_int(b))
            proc_push(p, val_int(val_get_int(a) + val_get_int(b)));
        else
            proc_push(p, val_from_double(val_to_double(a) + val_to_double(b)));
        NEXT();
    }
    CASE(OP_SUB) {
        Val b = proc_pop(p);
        Val a = proc_pop(p);
        /* int-int stays int; else float with non-numeric degrading to 0.0
         * (see OP_ADD note — matches golden reference). */
        if (val_is_int(a) && val_is_int(b))
            proc_push(p, val_int(val_get_int(a) - val_get_int(b)));
        else
            proc_push(p, val_from_double(val_to_double(a) - val_to_double(b)));
        NEXT();
    }
    CASE(OP_MUL) {
        Val b = proc_pop(p);
        Val a = proc_pop(p);
        /* int-int stays int; else float with non-numeric degrading to 0.0
         * (see OP_ADD note — matches golden reference). */
        if (val_is_int(a) && val_is_int(b))
            proc_push(p, val_int(val_get_int(a) * val_get_int(b)));
        else
            proc_push(p, val_from_double(val_to_double(a) * val_to_double(b)));
        NEXT();
    }
    CASE(OP_DIV) {
        Val b = proc_pop(p);
        Val a = proc_pop(p);
        if (val_is_int(a) && val_is_int(b)) {
            if (val_get_int(b) == 0) {
                /* Division by zero: kill only this process, deliver DOWN with
                 * reason 'divzero (process isolation — other procs continue). */
                int divzero = vm_intern_symbol(vm, "divzero");
                p->pc = pc;
                proc_die(vm, p, val_symbol((uint32_t)divzero));
                return -1;
            }
            proc_push(p, val_int(val_get_int(a) / val_get_int(b)));
            NEXT();
        }
        /* Mixed/non-int (e.g. a receive-bound string): float path, where
         * val_to_double degrades non-numerics to 0.0 (golden.py _binop —
         * only the both-int case stays on the integer path, so a dynamic
         * operand can no longer reach val_get_int and read its NaN-box
         * payload as an int; issue #158). Division by zero yields ±inf. */
        proc_push(p, val_from_double(val_to_double(a) / val_to_double(b)));
        NEXT();
    }
    CASE(OP_MOD) {
        Val b = proc_pop(p);
        Val a = proc_pop(p);
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
            proc_die(vm, p, val_symbol((uint32_t)arithtype));
            return -1;
        }
        if (val_get_int(b) == 0) {
            int divzero = vm_intern_symbol(vm, "divzero");
            p->pc = pc;
            proc_die(vm, p, val_symbol((uint32_t)divzero));
            return -1;
        }
        proc_push(p, val_int(val_get_int(a) % val_get_int(b)));
        NEXT();
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
    CASE(OP_EQ) {
        Val b = proc_pop(p);
        Val a = proc_pop(p);
        int eq;
        if (cmp_numeric_path(a, b))
            eq = val_to_double(a) == val_to_double(b);
        else
            eq = val_equal(a, b);
        proc_push(p, eq ? val_true() : val_false());
        NEXT();
    }
    CASE(OP_NE) {
        Val b = proc_pop(p);
        Val a = proc_pop(p);
        int ne;
        if (cmp_numeric_path(a, b))
            ne = val_to_double(a) != val_to_double(b);
        else
            ne = !val_equal(a, b);
        proc_push(p, ne ? val_true() : val_false());
        NEXT();
    }
    CASE(OP_LT) {
        Val b = proc_pop(p);
        Val a = proc_pop(p);
        int cmp;
        if (cmp_numeric_path(a, b))
            cmp = val_to_double(a) < val_to_double(b);
        else
            cmp = val_is_int(a) && val_is_int(b) && (val_get_int(a) < val_get_int(b));
        proc_push(p, cmp ? val_true() : val_false());
        NEXT();
    }
    CASE(OP_LE) {
        Val b = proc_pop(p);
        Val a = proc_pop(p);
        int cmp;
        if (cmp_numeric_path(a, b))
            cmp = val_to_double(a) <= val_to_double(b);
        else
            cmp = val_is_int(a) && val_is_int(b) && (val_get_int(a) <= val_get_int(b));
        proc_push(p, cmp ? val_true() : val_false());
        NEXT();
    }

    /* ---- type tests ---- */
    CASE(OP_IS_NIL) {
        Val v = proc_pop(p);
        proc_push(p, val_is_nil(v) ? val_true() : val_false());
        NEXT();
    }
    CASE(OP_IS_PAIR) {
        Val v = proc_pop(p);
        proc_push(p, val_is_pair(v) ? val_true() : val_false());
        NEXT();
    }
    CASE(OP_IS_INT) {
        Val v = proc_pop(p);
        proc_push(p, val_is_int(v) ? val_true() : val_false());
        NEXT();
    }
    CASE(OP_IS_STRING) {
        Val v = proc_pop(p);
        proc_push(p, val_is_string(v) ? val_true() : val_false());
        NEXT();
    }
    CASE(OP_IS_BYTES) {
        Val v = proc_pop(p);
        proc_push(p, val_is_bytes(v) ? val_true() : val_false());
        NEXT();
    }
    CASE(OP_IS_PID) {
        Val v = proc_pop(p);
        proc_push(p, val_is_pid(v) ? val_true() : val_false());
        NEXT();
    }

    /* ---- control flow ---- */
    CASE(OP_JUMP) {
        int32_t addr;
        memcpy(&addr, &p->code[pc], 4);
        pc = addr;
        NEXT();
    }
    CASE(OP_JUMP_IF_FALSE) {
        int32_t addr;
        memcpy(&addr, &p->code[pc], 4);
        pc += 4;
        Val v = proc_pop(p);
        if (val_is_nil(v) || v == val_false())
            pc = addr;
        NEXT();
    }
    CASE(OP_POP)
    proc_pop(p);
    NEXT();
    CASE(OP_DUP)
    proc_push(p, proc_peek(p, 0));
    NEXT();

    CASE(OP_PUSH_STRING) {
        int32_t len;
        memcpy(&len, &p->code[pc], 4);
        pc += 4;
        HeapString *s = (HeapString *)proc_heap_alloc(p, sizeof(HeapString) + len + 1);
        s->hdr.type = HEAP_STRING;
        s->hdr.flags = 0;
        s->len = len;
        memcpy(s->data, &p->code[pc], len);
        s->data[len] = '\0';
        pc += len;
        Val v = ((Val)TAG_STRING << 48) | (uint64_t)(uintptr_t)s;
        proc_push(p, v);
        NEXT();
    }

    CASE(OP_PUSH_FLOAT) {
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
        proc_push(p, val_float(strtod(buf, NULL)));
        NEXT();
    }

    /* ---- functions ---- */
    CASE(OP_CLOSURE) {
        int32_t fn_id, nfree;
        memcpy(&fn_id, &p->code[pc], 4);
        pc += 4;
        memcpy(&nfree, &p->code[pc], 4);
        pc += 4;
        if (nfree == 0) {
            /* No free vars — encode fn_id directly, no heap alloc */
            Val v = ((Val)TAG_CLOS_ID << 48) | (uint64_t)(uint32_t)fn_id;
            proc_push(p, v);
            NEXT();
        }
        /* proc_heap_alloc cannot fail (arena exhaustion is fatal), so no
         * OOM path is needed here. */
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
        proc_push(p, v);
        NEXT();
    }

    CASE(OP_CALL) {
        int32_t nargs;
        memcpy(&nargs, &p->code[pc], 4);
        pc += 4;
        /* Calling convention (compile_general_call): the caller pushes
         * the closure first, then arg0..argN-1, so on entry the closure
         * sits at sp+nargs and arg_j at sp+nargs-1-j. */
        Val closure_val = proc_peek(p, nargs);
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
        int caller_sp = p->sp + nargs + 1;
        int fp = caller_sp - nfree - nargs;
        /* Reserve that whole span at once (the old push-by-push code grew
         * stack as it went); mirrors proc_push's collision check. */
        proc_stack_reserve(p, fp - 4);

        /* Rearrange the operands in place — no C buffer. They occupy, low
         * to high index, [argN-1 .. arg0, closure]; reversing yields
         * [closure, arg0 .. argN-1], then shifting the block down by nfree
         * opens the slot above the args where the free vars go. No GC can
         * run mid-arrangement: this handler never calls proc_heap_alloc,
         * and proc_stack_reserve grows the arena only while the heap is
         * still empty, so no live heap Val is invalidated (#136). */
        Val *st = proc_stack(p);
        int sp = p->sp;
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
        p->sp = fp - 4;
        if ((closure_val >> 48) == TAG_CLOS_ID)
            pc = p->fn_table[(int)(closure_val & 0xFFFFFFFFFFFFULL)];
        else
            pc = p->fn_table[val_as_clos(closure_val)->entry];
        NEXT();
    }

    CASE(OP_TAIL_CALL) {
        int32_t nargs;
        memcpy(&nargs, &p->code[pc], 4);
        pc += 4;
        Val closure_val = proc_peek(p, nargs);
        if ((closure_val >> 48) != TAG_CLOS && (closure_val >> 48) != TAG_CLOS_ID) {
            fprintf(stderr, "error: cannot call non-function value\n");
            int notafn = vm_intern_symbol(vm, "notafunction");
            p->pc = pc;
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
        int sp = p->sp;
        int fp = caller_sp - nfree - nargs;

        if (closure_val == st[p->fp - 1] && fp == p->fp) {
            /* Self-recursive fast path: the callee is this very frame's
             * closure (same Val — TAG_CLOS and TAG_CLOS_ID alike, no deep
             * compare) and the geometry is unchanged, so free vars, frame
             * header and frame size are all reused as-is. Only the new
             * args move to fp+0 and sp resets to the empty body. */
            stack_reverse(st, sp, nargs);
            memmove(st + p->fp, st + sp, (size_t)nargs * sizeof(Val));
            p->sp = p->fp - 4;
        } else {
            /* General in-place rebuild: discard this frame's locals+body
             * (its 4 header slots are re-written at the new fp), reverse
             * the operands into the body, splice in the new closure's free
             * vars. Same no-GC discipline as OP_CALL: proc_stack_reserve
             * can only move the arena while the heap is empty (#136), and
             * this handler never calls proc_heap_alloc. */
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
            p->sp = fp - 4;
        }

        if ((closure_val >> 48) == TAG_CLOS_ID)
            pc = p->fn_table[(int)(closure_val & 0xFFFFFFFFFFFFULL)];
        else
            pc = p->fn_table[val_as_clos(closure_val)->entry];
        NEXT();
    }

    CASE(OP_RET) {
        Val ret_val = proc_pop(p);
        int caller_sp = (int)val_get_int(proc_stack(p)[p->fp - 4]);
        int old_fp = (int)val_get_int(proc_stack(p)[p->fp - 3]);
        int ret_addr = (int)val_get_int(proc_stack(p)[p->fp - 2]);

        p->sp = caller_sp;
        p->fp = old_fp;
        if (ret_addr < 0) {
            p->pc = pc;
            proc_die(vm, p, val_nil());
            return -1;
        }
        pc = ret_addr;
        proc_push(p, ret_val);
        NEXT();
    }

    CASE(OP_ENTER) {
        /* Reserve stack space for local variables.
         * Pushes nslots nil values so that GC sees safe values
         * and the parent's stack is not overwritten. */
        int32_t nslots;
        memcpy(&nslots, &p->code[pc], 4);
        pc += 4;
        for (int i = 0; i < nslots; i++)
            proc_push(p, val_nil());
        NEXT();
    }

    /* ---- actor primitives ---- */
    CASE(OP_SPAWN)
    CASE(OP_SPAWN_MAIN) {
        int32_t fn_id;
        memcpy(&fn_id, &p->code[pc], 4);
        pc += 4;

        Proc *np = proc_new(vm);
        proc_ensure_heap(np);
        np->fp = -4;
        np->sp = -8;
        proc_stack(np)[np->fp - 1] = val_nil();
        proc_stack(np)[np->fp - 2] = val_int(-1);
        proc_stack(np)[np->fp - 3] = val_int(0);
        proc_stack(np)[np->fp - 4] = val_int(np->sp);
        np->pc = np->fn_table[fn_id];
        runq_enqueue(vm, np->pid);
        /* Only OP_SPAWN_MAIN (compiler-spawned main()) sets main_pid.
         * Regular spawn from user code never changes main_pid. */
        if (op == OP_SPAWN_MAIN) {
            vm->main_pid = np->pid;
        }
        proc_push(p, val_pid(np->pid));
        NEXT();
    }

    CASE(OP_SPAWN_CLOS) {
        Val clos_val = proc_pop(p);
        Proc *np = proc_new(vm);
        proc_ensure_heap(np);

        /* Copy the whole closure into the CHILD's heap. The child must
         * never hold pointers into the parent's heap: the parent may GC
         * (moving objects) or die (freeing its heap) independently.
         * TAG_CLOS_ID immediates pass through val_deep_copy untouched.
         * Reserve room first: the fresh arena is tiny, and the gate-closed
         * copy cannot grow it. */
        proc_reserve_heap(np, val_calc_heap_size(clos_val));
        Val owned = val_deep_copy(np, clos_val);

        /* Set up frame: free vars at fp+0..fp+nfree-1, header at fp-1..fp-4.
         * `owned` and `clos` stay valid across the pushes: the pushes
         * perform no allocation (only proc_push), so the child's pending
         * collection is not honoured until its first real allocation, by
         * which point `owned` is rooted on the child's stack (#136). */
        int nfree = 0;
        HeapClosure *clos = NULL;
        if ((owned >> 48) == TAG_CLOS) {
            clos = val_as_clos(owned);
            nfree = clos->nfree;
        }
        np->sp = 0;
        for (int i = nfree - 1; i >= 0; i--)
            proc_push(np, clos->free[i]);
        /* push header */
        proc_push(np, owned);           /* fp-1 */
        proc_push(np, val_int(-1));     /* fp-2: ret_pc sentinel */
        proc_push(np, val_int(0));      /* fp-3: old_fp */
        proc_push(np, val_int(np->sp)); /* fp-4: caller_sp */
        np->fp = -nfree;                /* fp+0 = first free var */

        if ((clos_val >> 48) == TAG_CLOS_ID)
            np->pc = np->fn_table[(int)(clos_val & 0xFFFFFFFFFFFFULL)];
        else {
            HeapClosure *clos = val_as_clos(clos_val);
            np->pc = np->fn_table[clos->entry];
        }
        runq_enqueue(vm, np->pid);
        proc_push(p, val_pid(np->pid));
        NEXT();
    }

    CASE(OP_SEND) {
        Val pid_v = proc_pop(p); /* pid pushed last → on top */
        Val msg = proc_pop(p);   /* msg pushed first */
        /* The target pid comes from user code; bound it the same way
         * proc_new bounds the table, so a fabricated/stale pid cannot
         * index past procs[]. */
        uint32_t tpid = val_get_pid(pid_v);
        Proc *t = (tpid < (uint32_t)vm->procs_cap) ? vm->procs[tpid] : NULL;
        if (t && atomic_load(&t->state) != PROC_DEAD) {
            /* mbox_deliver serializes msg into a malloc'd fragment on the
             * sender's side and wakes the target under its mbox_lock if
             * blocked on recv (enqueue-at-most-once → Skynet invariant). */
            mbox_deliver(vm, t, msg);
        }
        proc_push(p, val_nil()); /* send returns nil to keep stack balanced */
        NEXT();
    }

    CASE(OP_RECV) {
        pthread_mutex_lock(&p->mbox_lock);
        if (p->mbox_count == 0) {
            /* Store the block state under mbox_lock: mbox_deliver checks
             * state under the same lock, so it cannot fall into the
             * check-then-block window and strand the message (same
             * invariant as OP_RECV_AFTER). */
            pc--; /* rewind so OP_RECV re-executes on resume */
            atomic_store(&p->state, PROC_WAIT_RECV);
            pthread_mutex_unlock(&p->mbox_lock);
            p->pc = pc;
            return -1;
        }
        pthread_mutex_unlock(&p->mbox_lock);
        proc_push(p, mbox_pop(p));
        /* mbox_pop's deep copy runs with the gate closed (its worklist is
         * off-stack), so a request may be pending; the popped message is
         * now rooted, so it is safe to honour it. */
        proc_gc_drain(p);
        NEXT();
    }

    /* Selective receive: peek the next mailbox fragment (without
     * removing it) deep-copied onto this proc's heap. The compiler
     * stores it in a temp slot and runs pattern code against it.
     * - If a fragment exists: push it and advance peek_index.
     * - If the mailbox is exhausted: rewind to this opcode, block on
     *   recv. peek_index is preserved so a resumed scan (after a new
     *   message arrives) only inspects unseen messages — already-skipped
     *   fragments don't match the (immutable) patterns, so skipping them
     *   forever is correct, and they stay for a future receive. */
    CASE(OP_RECV_PEEK) {
        pthread_mutex_lock(&p->mbox_lock);
        if (p->peek_index < p->mbox_count) {
            MsgFragment *frag = p->mbox_frag_head;
            for (int i = 0; i < p->peek_index; i++)
                frag = frag->next;
            /* Reserve before the gate-closed copy; see mbox_pop. The
             * collection may run under mbox_lock — it takes no locks, so
             * a concurrent sender only waits, never deadlocks. */
            proc_reserve_heap(p, val_calc_heap_size(frag->root));
            Val msg = val_deep_copy(p, frag->root);
            p->peek_index++;
            pthread_mutex_unlock(&p->mbox_lock);
            proc_push(p, msg);
            /* The copied message is rooted now; honour any collection the
             * gate-closed deep copy requested (same as OP_RECV). */
            proc_gc_drain(p);
        } else {
            /* Same invariant as OP_RECV: store the block state while
             * holding mbox_lock so a concurrent mbox_deliver cannot miss
             * the wake and strand the message. */
            pc--; /* re-execute OP_RECV_PEEK on wake */
            atomic_store(&p->state, PROC_WAIT_RECV);
            pthread_mutex_unlock(&p->mbox_lock);
            p->pc = pc;
            return -1;
        }
        NEXT();
    }

    /* A pattern matched: drop the fragment we just peeked (at
     * peek_index-1) from the mailbox and reset the scan cursor. The
     * matched message's heap copy was already consumed by the pattern
     * (bound to variables); the fragment itself is freed here. */
    CASE(OP_RECV_COMMIT) {
        int target = p->peek_index - 1;
        pthread_mutex_lock(&p->mbox_lock);
        if (target == 0) {
            MsgFragment *frag = p->mbox_frag_head;
            p->mbox_frag_head = frag->next;
            if (!p->mbox_frag_head)
                p->mbox_frag_tail = NULL;
            free(frag);
        } else {
            MsgFragment *prev = p->mbox_frag_head;
            for (int i = 0; i < target - 1; i++)
                prev = prev->next;
            MsgFragment *frag = prev->next;
            prev->next = frag->next;
            if (frag == p->mbox_frag_tail)
                p->mbox_frag_tail = prev;
            free(frag);
        }
        p->mbox_count--;
        p->peek_index = 0;
        pthread_mutex_unlock(&p->mbox_lock);
        NEXT();
    }

    CASE(OP_SELF)
    proc_push(p, val_pid((uint32_t)p->pid));
    NEXT();

    CASE(OP_MONITOR) {
        Val pid_v = proc_pop(p);
        uint32_t tpid = val_get_pid(pid_v);
        int ref = ++vm->next_ref;
        int alive = 0;
        /* Fetch the target and mutate its watcher array under procs_lock:
         * proc_die walks the same array under this lock (strictly after
         * setting PROC_DEAD), so without the lock a concurrent death tears
         * the realloc'd array (issue #123). */
        pthread_mutex_lock(&vm->procs_lock);
        Proc *t = (tpid < (uint32_t)vm->procs_cap) ? vm->procs[tpid] : NULL;
        if (t && atomic_load(&t->state) != PROC_DEAD) {
            /* Normal path: join watchers, DOWN sent when target dies */
            if (t->watcher_count >= t->watcher_cap) {
                t->watcher_cap = t->watcher_cap ? t->watcher_cap * 2 : 4;
                t->watchers = realloc(t->watchers, t->watcher_cap * sizeof(int));
                t->watcher_refs = realloc(t->watcher_refs, t->watcher_cap * sizeof(Val));
            }
            t->watchers[t->watcher_count] = p->pid;
            t->watcher_refs[t->watcher_count] = val_int(ref);
            t->watcher_count++;
            alive = 1;
        }
        pthread_mutex_unlock(&vm->procs_lock);
        if (!alive) {
            /* Target already dead or nonexistent: deliver DOWN immediately.
             * Only THIS ref is at stake: we did not insert it, so proc_die's
             * iteration cannot produce a duplicate. The nested val_pair
             * chain keeps each intermediate pair only in a C local across
             * the next allocation, so no collection may run inside it:
             * reserve its room first, then close the gate. */
            proc_reserve_heap(p, 4 * ta_heap_object_size(sizeof(HeapPair)));
            proc_gc_enter(p);
            int down_sym = vm_intern_symbol(vm, "DOWN");
            int noproc_sym = vm_intern_symbol(vm, "noproc");
            Val msg = val_pair(
                p, val_symbol((uint32_t)down_sym),
                val_pair(p, val_int(ref),
                         val_pair(p, val_pid(tpid),
                                  val_pair(p, val_symbol((uint32_t)noproc_sym), val_nil()))));
            mbox_deliver(vm, p, msg);
            proc_gc_reopen(p); /* msg was serialized out; nothing is at risk */
        }
        /* No double-check needed anymore: if the target died after we
         * released the lock, proc_die's iteration - under the same lock,
         * and only after PROC_DEAD is set - necessarily observes our
         * entry. The old racy re-check assumed an unlocked insert that a
         * concurrent death could skip (issue #123). */
        proc_push(p, val_int(ref));
        NEXT();
    }

    /* recv_after(ms): wait up to ms for the next mailbox message.
     * Message available first -> pop and return it (FIFO, same as
     * OP_RECV). Deadline passes first -> return nil; the mailbox is
     * untouched (Erlang/Gleam semantics: a timeout never consumes
     * messages). The atomic deadline tracks the armed state: -1 = the
     * ms operand is still on the stack (arm on this execution);
     * RECV_AFTER_EXPIRED = the scheduler's deadline scan already fired
     * the timeout while we were blocked — return nil without touching
     * the mailbox even if a message arrived after expiry; >= 0 = armed
     * deadline, re-check mbox/timeout. On block we rewind pc so this
     * opcode re-executes when woken — by mbox_deliver (message wins,
     * deadline cleared) or by the scheduler's deadline scan (mailbox
     * still empty -> nil). */
    CASE(OP_RECV_AFTER) {
        if (atomic_load(&p->recv_deadline_ms) == RECV_AFTER_EXPIRED) {
            atomic_store(&p->recv_deadline_ms, -1);
            atomic_fetch_sub(&vm->recv_armed, 1);
            proc_push(p, val_nil());
            NEXT();
        }
        if (atomic_load(&p->recv_deadline_ms) < 0) {
            Val ms_v = proc_pop(p);
            atomic_store(&p->recv_deadline_ms, net_now_ms() + val_get_int(ms_v));
            atomic_fetch_add(&vm->recv_armed, 1);
            vm_wake_poller(vm);
        }
        pthread_mutex_lock(&p->mbox_lock);
        if (p->mbox_count > 0) {
            pthread_mutex_unlock(&p->mbox_lock);
            atomic_store(&p->recv_deadline_ms, -1);
            atomic_fetch_sub(&vm->recv_armed, 1);
            proc_push(p, mbox_pop(p));
            proc_gc_drain(p); /* message rooted; see OP_RECV */
            NEXT();
        }
        if (net_now_ms() >= atomic_load(&p->recv_deadline_ms)) {
            pthread_mutex_unlock(&p->mbox_lock);
            atomic_store(&p->recv_deadline_ms, -1);
            atomic_fetch_sub(&vm->recv_armed, 1);
            proc_push(p, val_nil());
            NEXT();
        }
        /* Block. State is stored under mbox_lock so a concurrent
         * mbox_deliver (which checks state under the same lock) cannot
         * fall into the check-then-block window and strand the message. */
        pc--; /* rewind so OP_RECV_AFTER re-executes on resume */
        atomic_store(&p->state, PROC_WAIT_RECV);
        pthread_mutex_unlock(&p->mbox_lock);
        p->pc = pc;
        return -1;
    }

    /* ---- built-in ---- */
    CASE(OP_HALT)
    vm->eval_result = proc_peek(p, 0);
    p->pc = pc;
    proc_die(vm, p, val_nil());
    return -1;

    CASE(OP_CCALL_NAME) {
        int pc_start = pc - 1; /* save for rewind on yield */
        int sym_idx;
        memcpy(&sym_idx, p->code + pc, 4);
        pc += 4;
        uint8_t nc = p->code[pc++];
        if (sym_idx < 0 || sym_idx >= vm->sym_count) {
            for (int i = 0; i < nc; i++)
                proc_pop(p);
            proc_push(p, val_nil());
            NEXT();
        }
        if (nc > 64) {
            for (int i = 0; i < nc; i++)
                proc_pop(p);
            proc_push(p, val_nil());
            NEXT();
        }
        const char *name = vm->symbols[sym_idx];
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
                    proc_pop(p);
                proc_push(p, val_nil());
                NEXT();
            }
        }
        Val args[64];
        for (int i = nc - 1; i >= 0; i--)
            args[i] = proc_pop(p);
        tls_current_proc = p;
        p->yield_requested = 0;
        p->die_requested = 0;
        /* The C module may allocate on p's heap while holding Vals it got
         * from args (or its own root-free state) in C locals, so no
         * collection may run inside the callback: close the gate. */
        proc_gc_enter(p);
        p->in_ccall = 1;
        Val result = vm->cfuncs[cfidx].fn(vm, args, nc);
        if (p->die_requested) {
            /* Builtin raised a runtime error (vm_die): kill this proc with
             * the reason symbol, exactly like an opcode type error. */
            p->die_requested = 0;
            Val reason = p->die_reason;
            p->in_ccall = 0;
            proc_gc_leave(p);
            p->pc = pc;
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
                proc_push(p, args[i]);
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
        proc_push(p, result);
        /* result is rooted now; a callback that allocated heavily left a
         * pending request the gate suppressed, so honour it here rather
         * than let a cfunc loop grow the heap unbounded. */
        proc_gc_reopen(p);
        NEXT();
    }
#if TA_COMPUTED_GOTO
    /* Reached only through DISPATCH()'s bounds check: an opcode >= OP_COUNT
     * has no table entry, so it is reported here rather than dereferenced. */
CASE_OP_UNKNOWN:
#else
        default:
#endif
    return vm_unknown_opcode(vm, p, op, pc);
#if !TA_COMPUTED_GOTO
} /* switch (op) */
TICK_FETCH();
} /* for (;;) */
#endif
/* clang-format on */
}
