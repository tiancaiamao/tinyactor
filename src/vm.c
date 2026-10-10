/*
 * vm.c — shared VM helpers: value equality, yield/die, stack walking, printer
 */

#include "ta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Thread-local current process — set by the host run loop (tavm run_proc)
 * while a proc executes. */
__thread Proc *tls_current_proc = NULL;

/* Equality for OP_EQ/OP_NE. Strings compare by content (HeapString holds
 * len + NUL-terminated data); ints/symbols/nil/true/false compare by their
 * NaN-boxed value (int payload is direct, symbols are interned); everything
 * else (pair/closure/bytes/pid) compares by pointer identity. */
int val_equal(Val a, Val b) {
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
int cmp_numeric_path(Val a, Val b) {
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
    /* 图像特定行走（tavm 注册）：lisp 图的帧在 LState 返回栈，不在 p->mem。 */
    if (vm->walk_stack)
        return vm->walk_stack(vm, p, out, max_depth);
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
