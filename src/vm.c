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

int vm_run_proc(VM *vm, Proc *p, int reductions) {
    /* pc lives in a C local for the whole run and is written back to p->pc
     * only at the exit points (suspend / die / budget exhausted) and before
     * prof_collect, which resolves the leaf frame from p->pc. */
    int pc = p->pc;
    int prof_on = vm->prof_on;
    uint64_t prof_last = 0;
    if (prof_on)
        prof_last = prof_now_ns();

    for (int r = 0; r < reductions; r++) {
        /* Instruction boundary: every gate region must have been closed again
         * by the handler that opened it, so a nonzero depth here means some
         * exit forgot its proc_gc_leave(). Asserting it makes that a
         * deterministic crash under TA_GC_STRESS=1 instead of a later silent
         * UAF. */
        assert(p->gc_gate == 0);
        uint8_t op = p->code[pc++];

        switch (op) {

        /* ---- stack constants ---- */
        case OP_PUSH_NIL:
            proc_push(p, val_nil());
            break;
        case OP_PUSH_TRUE:
            proc_push(p, val_true());
            break;
        case OP_PUSH_FALSE:
            proc_push(p, val_false());
            break;

        case OP_PUSH_INT8: {
            int8_t i8 = (int8_t)p->code[pc++];
            proc_push(p, val_int(i8));
            break;
        }
        case OP_PUSH_INT: {
            int64_t i64;
            memcpy(&i64, &p->code[pc], 8);
            pc += 8;
            proc_push(p, val_int(i64));
            break;
        }
        case OP_PUSH_SYM: {
            int32_t idx;
            memcpy(&idx, &p->code[pc], 4);
            pc += 4;
            proc_push(p, val_symbol((uint32_t)idx));
            break;
        }

        /* ---- local variables ---- */
        case OP_LOAD: {
            int32_t off;
            memcpy(&off, &p->code[pc], 4);
            pc += 4;
            proc_push(p, proc_stack(p)[p->fp + off]);
            break;
        }
        case OP_STORE: {
            int32_t off;
            memcpy(&off, &p->code[pc], 4);
            pc += 4;
            proc_stack(p)[p->fp + off] = proc_pop(p);
            break;
        }

        /* ---- pair ---- */
        case OP_CONS: {
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
            break;
        }
        case OP_CAR: {
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
            break;
        }
        case OP_CDR: {
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
            break;
        }

            /* ---- arithmetic ---- */
        /* Mixed int/float: if EITHER operand is a float, the operation is done in
         * double precision and the result is a float (never narrowed back to int),
         * so `1 + 1.5` → 2.5 and `3.0 / 2` → 1.5. Pure int stays int (3/2 == 1). */
        case OP_ADD: {
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
            break;
        }
        case OP_SUB: {
            Val b = proc_pop(p);
            Val a = proc_pop(p);
            /* int-int stays int; else float with non-numeric degrading to 0.0
             * (see OP_ADD note — matches golden reference). */
            if (val_is_int(a) && val_is_int(b))
                proc_push(p, val_int(val_get_int(a) - val_get_int(b)));
            else
                proc_push(p, val_from_double(val_to_double(a) - val_to_double(b)));
            break;
        }
        case OP_MUL: {
            Val b = proc_pop(p);
            Val a = proc_pop(p);
            /* int-int stays int; else float with non-numeric degrading to 0.0
             * (see OP_ADD note — matches golden reference). */
            if (val_is_int(a) && val_is_int(b))
                proc_push(p, val_int(val_get_int(a) * val_get_int(b)));
            else
                proc_push(p, val_from_double(val_to_double(a) * val_to_double(b)));
            break;
        }
        case OP_DIV: {
            Val b = proc_pop(p);
            Val a = proc_pop(p);
            if (val_is_float(a) || val_is_float(b)) {
                /* Float division: IEEE semantics — division by zero yields ±inf
                 * (never traps), so 1.0/0.0 → inf. */
                proc_push(p, val_from_double(val_to_double(a) / val_to_double(b)));
                break;
            }
            if (val_get_int(b) == 0) {
                /* Division by zero: kill only this process, deliver DOWN with
                 * reason 'divzero (process isolation — other procs continue). */
                int divzero = vm_intern_symbol(vm, "divzero");
                p->pc = pc;
                proc_die(vm, p, val_symbol((uint32_t)divzero));
                return -1;
            }
            proc_push(p, val_int(val_get_int(a) / val_get_int(b)));
            break;
        }
        case OP_MOD: {
            Val b = proc_pop(p);
            Val a = proc_pop(p);
            if (val_get_int(b) == 0) {
                int divzero = vm_intern_symbol(vm, "divzero");
                p->pc = pc;
                proc_die(vm, p, val_symbol((uint32_t)divzero));
                return -1;
            }
            proc_push(p, val_int(val_get_int(a) % val_get_int(b)));
            break;
        }

            /* ---- comparison ---- */
            /* Mixed int/float comparisons are numeric: 3 == 3.0 → true,
             * 2.5 < 3 → true. Pure non-numeric operands keep the old behavior
             * (bit/content equality; LT/LE false for non-ints). */
        case OP_EQ: {
            Val b = proc_pop(p);
            Val a = proc_pop(p);
            int eq;
            if (val_is_float(a) || val_is_float(b))
                eq = val_to_double(a) == val_to_double(b);
            else
                eq = val_equal(a, b);
            proc_push(p, eq ? val_true() : val_false());
            break;
        }
        case OP_NE: {
            Val b = proc_pop(p);
            Val a = proc_pop(p);
            int ne;
            if (val_is_float(a) || val_is_float(b))
                ne = val_to_double(a) != val_to_double(b);
            else
                ne = !val_equal(a, b);
            proc_push(p, ne ? val_true() : val_false());
            break;
        }
        case OP_LT: {
            Val b = proc_pop(p);
            Val a = proc_pop(p);
            int cmp;
            if (val_is_float(a) || val_is_float(b))
                cmp = val_to_double(a) < val_to_double(b);
            else
                cmp = val_is_int(a) && val_is_int(b) && (val_get_int(a) < val_get_int(b));
            proc_push(p, cmp ? val_true() : val_false());
            break;
        }
        case OP_LE: {
            Val b = proc_pop(p);
            Val a = proc_pop(p);
            int cmp;
            if (val_is_float(a) || val_is_float(b))
                cmp = val_to_double(a) <= val_to_double(b);
            else
                cmp = val_is_int(a) && val_is_int(b) && (val_get_int(a) <= val_get_int(b));
            proc_push(p, cmp ? val_true() : val_false());
            break;
        }

        /* ---- type tests ---- */
        case OP_IS_NIL: {
            Val v = proc_pop(p);
            proc_push(p, val_is_nil(v) ? val_true() : val_false());
            break;
        }
        case OP_IS_PAIR: {
            Val v = proc_pop(p);
            proc_push(p, val_is_pair(v) ? val_true() : val_false());
            break;
        }
        case OP_IS_INT: {
            Val v = proc_pop(p);
            proc_push(p, val_is_int(v) ? val_true() : val_false());
            break;
        }
        case OP_IS_STRING: {
            Val v = proc_pop(p);
            proc_push(p, val_is_string(v) ? val_true() : val_false());
            break;
        }
        case OP_IS_BYTES: {
            Val v = proc_pop(p);
            proc_push(p, val_is_bytes(v) ? val_true() : val_false());
            break;
        }
        case OP_IS_PID: {
            Val v = proc_pop(p);
            proc_push(p, val_is_pid(v) ? val_true() : val_false());
            break;
        }

        /* ---- control flow ---- */
        case OP_JUMP: {
            int32_t addr;
            memcpy(&addr, &p->code[pc], 4);
            pc = addr;
            break;
        }
        case OP_JUMP_IF_FALSE: {
            int32_t addr;
            memcpy(&addr, &p->code[pc], 4);
            pc += 4;
            Val v = proc_pop(p);
            if (val_is_nil(v) || v == val_false())
                pc = addr;
            break;
        }
        case OP_POP:
            proc_pop(p);
            break;
        case OP_DUP:
            proc_push(p, proc_peek(p, 0));
            break;

        case OP_PUSH_STRING: {
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
            break;
        }

        case OP_PUSH_FLOAT: {
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
            break;
        }

            /* ---- functions ---- */
        case OP_CLOSURE: {
            int32_t fn_id, nfree;
            memcpy(&fn_id, &p->code[pc], 4);
            pc += 4;
            memcpy(&nfree, &p->code[pc], 4);
            pc += 4;
            if (nfree == 0) {
                /* No free vars — encode fn_id directly, no heap alloc */
                Val v = ((Val)TAG_CLOS_ID << 48) | (uint64_t)(uint32_t)fn_id;
                proc_push(p, v);
                break;
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
            break;
        }

        case OP_CALL: {
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
                    st[sp - nfree + nargs + 1 + i] = clos->free[i];
            }
            /* closure now sits at fp-1; write the three return-context slots. */
            st[sp - nfree - 1] = val_int(pc);        /* ret_pc    (fp-2) */
            st[sp - nfree - 2] = val_int(p->fp);     /* old_fp    (fp-3) */
            st[sp - nfree - 3] = val_int(caller_sp); /* caller_sp (fp-4) */

            p->fp = fp;
            p->sp = fp - 4;
            if ((closure_val >> 48) == TAG_CLOS_ID)
                pc = p->fn_table[(int)(closure_val & 0xFFFFFFFFFFFFULL)];
            else
                pc = p->fn_table[val_as_clos(closure_val)->entry];
            break;
        }

        case OP_TAIL_CALL: {
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
                p->fp = fp;
                p->sp = fp - 4;
            }

            if ((closure_val >> 48) == TAG_CLOS_ID)
                pc = p->fn_table[(int)(closure_val & 0xFFFFFFFFFFFFULL)];
            else
                pc = p->fn_table[val_as_clos(closure_val)->entry];
            break;
        }

        case OP_RET: {
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
            break;
        }

        case OP_ENTER: {
            /* Reserve stack space for local variables.
             * Pushes nslots nil values so that GC sees safe values
             * and the parent's stack is not overwritten. */
            int32_t nslots;
            memcpy(&nslots, &p->code[pc], 4);
            pc += 4;
            for (int i = 0; i < nslots; i++)
                proc_push(p, val_nil());
            break;
        }

            /* ---- actor primitives ---- */
        case OP_SPAWN:
        case OP_SPAWN_MAIN: {
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
            break;
        }

        case OP_SPAWN_CLOS: {
            Val clos_val = proc_pop(p);
            Proc *np = proc_new(vm);
            proc_ensure_heap(np);

            /* Copy the whole closure into the CHILD's heap. The child must
             * never hold pointers into the parent's heap: the parent may GC
             * (moving objects) or die (freeing its heap) independently.
             * TAG_CLOS_ID immediates pass through val_deep_copy untouched. */
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
            break;
        }

        case OP_SEND: {
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
            break;
        }

        case OP_RECV: {
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
            break;
        }

        /* Selective receive: peek the next mailbox fragment (without
         * removing it) deep-copied onto this proc's heap. The compiler
         * stores it in a temp slot and runs pattern code against it.
         * - If a fragment exists: push it, advance peek_index, and reset
         *   match_ok so the following pattern sequence starts clean.
         * - If the mailbox is exhausted: rewind to this opcode, block on
         *   recv. peek_index is preserved so a resumed scan (after a new
         *   message arrives) only inspects unseen messages — already-skipped
         *   fragments don't match the (immutable) patterns, so skipping them
         *   forever is correct, and they stay for a future receive. */
        case OP_RECV_PEEK: {
            p->match_ok = 1;
            pthread_mutex_lock(&p->mbox_lock);
            if (p->peek_index < p->mbox_count) {
                MsgFragment *frag = p->mbox_frag_head;
                for (int i = 0; i < p->peek_index; i++)
                    frag = frag->next;
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
            break;
        }

        /* A pattern matched: drop the fragment we just peeked (at
         * peek_index-1) from the mailbox and reset the scan cursor. The
         * matched message's heap copy was already consumed by the pattern
         * (bound to variables); the fragment itself is freed here. */
        case OP_RECV_COMMIT: {
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
            break;
        }

        case OP_SELF:
            proc_push(p, val_pid((uint32_t)p->pid));
            break;

        case OP_MONITOR: {
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
                 * close the gate. */
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
            break;
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
        case OP_RECV_AFTER: {
            if (atomic_load(&p->recv_deadline_ms) == RECV_AFTER_EXPIRED) {
                atomic_store(&p->recv_deadline_ms, -1);
                atomic_fetch_sub(&vm->recv_armed, 1);
                proc_push(p, val_nil());
                break;
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
                break;
            }
            if (net_now_ms() >= atomic_load(&p->recv_deadline_ms)) {
                pthread_mutex_unlock(&p->mbox_lock);
                atomic_store(&p->recv_deadline_ms, -1);
                atomic_fetch_sub(&vm->recv_armed, 1);
                proc_push(p, val_nil());
                break;
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
        case OP_PRINT: {
            Val v = proc_pop(p);
            print_val(vm, v);
            printf("\n");
            fflush(stdout);
            proc_push(p, val_nil());
            break;
        }
        case OP_HALT:
            vm->eval_result = proc_peek(p, 0);
            p->pc = pc;
            proc_die(vm, p, val_nil());
            return -1;

        /* ---- pattern matching ---- */
        case OP_MATCH_INT: {
            int64_t expected;
            memcpy(&expected, &p->code[pc], 8);
            pc += 8;
            if (!p->match_ok)
                break;
            Val v = proc_pop(p);
            if (val_is_int(v) && val_get_int(v) == expected) {
                /* consumed */
            } else {
                proc_push(p, v);
                p->match_ok = 0;
            }
            break;
        }
        case OP_MATCH_SYM: {
            int32_t idx;
            memcpy(&idx, &p->code[pc], 4);
            pc += 4;
            if (!p->match_ok)
                break;
            Val v = proc_pop(p);
            if (val_is_symbol(v) && val_get_symbol(v) == (uint32_t)idx) {
                /* consumed */
            } else {
                proc_push(p, v);
                p->match_ok = 0;
            }
            break;
        }
        case OP_MATCH_STR: {
            int32_t slen;
            memcpy(&slen, &p->code[pc], 4);
            pc += 4;
            const char *sdata = (const char *)&p->code[pc];
            pc += slen;
            if (!p->match_ok)
                break;
            Val v = proc_pop(p);
            if (val_is_string(v)) {
                HeapString *hs = val_get_string(v);
                if (hs->len == slen && memcmp(hs->data, sdata, slen) == 0) {
                    /* consumed */
                } else {
                    proc_push(p, v);
                    p->match_ok = 0;
                }
            } else {
                proc_push(p, v);
                p->match_ok = 0;
            }
            break;
        }
        case OP_MATCH_NIL: {
            if (!p->match_ok)
                break;
            Val v = proc_pop(p);
            if (val_is_nil(v)) {
                /* consumed */
            } else {
                proc_push(p, v);
                p->match_ok = 0;
            }
            break;
        }
        case OP_MATCH_PAIR: {
            if (!p->match_ok)
                break;
            Val v = proc_pop(p);
            if (val_is_pair(v)) {
                proc_push(p, val_get_cdr(v));
                proc_push(p, val_get_car(v));
            } else {
                proc_push(p, v);
                p->match_ok = 0;
            }
            break;
        }
        case OP_MATCH_JUMP: {
            int32_t addr;
            memcpy(&addr, &p->code[pc], 4);
            pc += 4;
            if (!p->match_ok) {
                pc = addr;
                p->match_ok = 1;
            }
            break;
        }

        /* ---- string builtins ---- */
        case OP_STR_LEN: {
            Val s = proc_pop(p);
            if (val_tag(s) != TAG_STRING) {
                proc_push(p, val_nil());
                break;
            }
            HeapString *hs = val_get_string(s);
            proc_push(p, val_int(hs->len));
            break;
        }
        case OP_STR_CONCAT: {
            Val s2 = proc_pop(p);
            Val s1 = proc_pop(p);
            if (val_tag(s1) != TAG_STRING || val_tag(s2) != TAG_STRING) {
                proc_push(p, val_nil());
                break;
            }
            HeapString *h1 = val_get_string(s1);
            HeapString *h2 = val_get_string(s2);
            /* Extract data to C locals BEFORE any allocation (GC safety) */
            int len1 = h1->len, len2 = h2->len;
            int total_len = len1 + len2;
            char *tmp = malloc(total_len + 1);
            if (!tmp) {
                proc_push(p, val_nil());
                break;
            }
            memcpy(tmp, h1->data, len1);
            memcpy(tmp + len1, h2->data, len2);
            tmp[total_len] = '\0';
            Val result = val_string(p, tmp, total_len);
            free(tmp);
            proc_push(p, result);
            break;
        }
        case OP_STR_SLICE: {
            Val vend = proc_pop(p);
            Val vstart = proc_pop(p);
            Val s = proc_pop(p);
            if (val_tag(s) != TAG_STRING) {
                proc_push(p, val_nil());
                break;
            }
            HeapString *hs = val_get_string(s);
            int start = (int)val_get_int(vstart);
            int end = (int)val_get_int(vend);
            if (start < 0)
                start = 0;
            if (end > hs->len)
                end = hs->len;
            if (start >= end) {
                proc_push(p, val_string(p, "", 0));
                break;
            }
            /* Extract before allocating */
            int slen = end - start;
            char tmp[slen + 1];
            memcpy(tmp, hs->data + start, slen);
            tmp[slen] = '\0';
            Val result = val_string(p, tmp, slen);
            proc_push(p, result);
            break;
        }
        case OP_STR_EQ: {
            Val s2 = proc_pop(p);
            Val s1 = proc_pop(p);
            if (val_tag(s1) != TAG_STRING || val_tag(s2) != TAG_STRING) {
                proc_push(p, val_nil());
                break;
            }
            HeapString *h1 = val_get_string(s1);
            HeapString *h2 = val_get_string(s2);
            int eq = (h1->len == h2->len && memcmp(h1->data, h2->data, h1->len) == 0);
            proc_push(p, eq ? val_true() : val_nil());
            break;
        }
        /* 54 was OP_CCALL (index-based) — removed, use OP_CCALL_NAME */
        case OP_CCALL_NAME: {
            int pc_start = pc - 1; /* save for rewind on yield */
            int sym_idx;
            memcpy(&sym_idx, p->code + pc, 4);
            pc += 4;
            uint8_t nc = p->code[pc++];
            if (sym_idx < 0 || sym_idx >= vm->sym_count) {
                for (int i = 0; i < nc; i++)
                    proc_pop(p);
                proc_push(p, val_nil());
                break;
            }
            if (nc > 64) {
                for (int i = 0; i < nc; i++)
                    proc_pop(p);
                proc_push(p, val_nil());
                break;
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
                    int n = snprintf(mod_path, sizeof(mod_path), "lib/%.*s.%s", mod_len, name, ext);
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
                    break;
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
            Val result = vm->cfuncs[cfidx].fn(vm, args, nc);
            if (p->die_requested) {
                /* Builtin raised a runtime error (vm_die): kill this proc with
                 * the reason symbol, exactly like an opcode type error. */
                p->die_requested = 0;
                Val reason = p->die_reason;
                proc_gc_leave(p);
                p->pc = pc;
                proc_die(vm, p, reason);
                return -1;
            }
            if (p->yield_requested) {
                /* Re-root args before reopening the gate: the callback left
                 * them in C locals, but the stack is the root set. */
                for (int i = 0; i < nc; i++)
                    proc_push(p, args[i]);
                proc_gc_reopen(p);
                atomic_store(&p->state, PROC_WAIT_IO);
                p->pc = pc_start;
                return -1;
            }
            proc_push(p, result);
            /* result is rooted now; a callback that allocated heavily left a
             * pending request the gate suppressed, so honour it here rather
             * than let a cfunc loop grow the heap unbounded. */
            proc_gc_reopen(p);
            break;
        }

        default:
            fprintf(stderr, "vm_run_proc: unknown opcode %d at pc=%d\n", op, pc - 1);
            int badop = vm_intern_symbol(vm, "badopcode");
            p->pc = pc;
            proc_die(vm, p, val_symbol((uint32_t)badop));
            return -1;
        }

        /* Sampling profiler: attribute the last 64 instructions to the TA
         * stack observed at this boundary — same sampling points as before,
         * now driven by this loop's counter (Lua count-hook style). p->pc is
         * written back first: vm_walk_stack names the leaf frame from it. */
        if (prof_on && (r & 63) == 63) {
            p->pc = pc;
            uint64_t now = prof_now_ns();
            prof_collect(vm, p, now - prof_last);
            prof_last = now;
        }
    }

    p->pc = pc;
    return 0;
}
