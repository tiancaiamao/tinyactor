/*
 * val.c — NaN-boxing value operations for TinyActor
 *
 * Encoding (64-bit):
 *   Normal double   → stored as-is (top 16 bits outside the tag range)
 *   Non-double types → high 16 bits = tag, low 48 bits = payload
 *
 *   TAG_INT      0xFFF1  → low 48 = sign-extended int48
 *   TAG_NIL      0xFFF2  → no payload
 *   TAG_TRUE     0xFFF3  → no payload
 *   TAG_FALSE    0xFFF4  → no payload
 *   TAG_SYM      0xFFF5  → low 32 = symbol table index
 *   TAG_PAIR     0xFFF6  → low 48 = heap pointer
 *   TAG_PID      0xFFF7  → low 32 = pid
 *   TAG_CLOS     0xFFF8  → low 48 = heap pointer
 *   TAG_STRING   0xFFF9  → low 48 = heap pointer
 *   TAG_BYTES    0xFFFA  → low 48 = heap pointer
 *   TAG_CLOS_ID  0xFFFB  → low 48 = direct fn_id
 *   TAG_RESOURCE 0xFFFC  → low 48 = heap pointer (HeapResource header)
 *
 * Float discrimination: a value is a float iff its top 16 bits fall outside
 * the tag range [TAG_FIRST, TAG_LAST]. The tags live in the negative-NaN
 * exponent zone, which finite doubles cannot reach (exponent <= 0x7FE ⇒ top
 * 16 bits <= 0xFFEF), and -Inf (0xFFF0...) sits below TAG_FIRST (0xFFF1) —
 * so every finite double and both infinities are stored as-is. The only
 * remaining colliders are NaN bit patterns: val_float / val_from_double
 * canonicalize them to VAL_CANON_NAN at box time (issue #169).
 */

#include "ta.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * Internal helpers
 * ============================================================ */

/* Build a NaN-boxed value: tag in bits [63:48], payload in low 48 bits. */
static inline Val box_tag_payload(uint16_t tag, uint64_t payload) {
    return ((uint64_t)tag << 48) | (payload & 0x0000FFFFFFFFFFFFULL);
}

/* Extract the low 48 bits as unsigned. */
static inline uint64_t val_payload48(Val v) { return v & 0x0000FFFFFFFFFFFFULL; }

/* Extract the low 32 bits as unsigned. */
static inline uint32_t val_payload32(Val v) { return (uint32_t)(v & 0xFFFFFFFFULL); }

/* ============================================================
 * Value constructors
 * ============================================================ */

/* val_int is static inline in ta_inline.h (see the Value helpers section
 * there) — it builds the int result of every arithmetic opcode. */

Val val_float(double d) {
    /* Same encoding rule as the hot-path inline val_from_double
     * (ta_inline.h): doubles stored as-is, NaNs canonicalized (see the
     * float-discrimination note in the header comment). */
    return val_from_double(d);
}

Val val_nil(void) { return box_tag_payload(TAG_NIL, 0); }
Val val_true(void) { return box_tag_payload(TAG_TRUE, 0); }
Val val_false(void) { return box_tag_payload(TAG_FALSE, 0); }

Val val_symbol(uint32_t idx) { return box_tag_payload(TAG_SYM, (uint64_t)idx); }

Val val_pid(uint32_t pid) { return box_tag_payload(TAG_PID, (uint64_t)pid); }

/* ============================================================
 * Heap-allocated constructors (require process context)
 * ============================================================ */

/* Constructors cannot fail: proc_heap_alloc aborts on arena exhaustion
 * instead of handing back a half-built value. */
Val val_pair(Proc *p, Val car, Val cdr) {
    HeapPair *hp = (HeapPair *)proc_heap_alloc(p, sizeof(HeapPair));
    hp->hdr.type = HEAP_PAIR;
    hp->hdr.flags = 0;
    hp->car = car;
    hp->cdr = cdr;
    return box_tag_payload(TAG_PAIR, (uint64_t)(uintptr_t)hp);
}

Val val_string(Proc *p, const char *data, int len) {
    int total = sizeof(HeapString) + len + 1; /* +1 for NUL */
    HeapString *hs = (HeapString *)proc_heap_alloc(p, total);
    hs->hdr.type = HEAP_STRING;
    hs->hdr.flags = 0;
    hs->len = len;
    memcpy(hs->data, data, len);
    hs->data[len] = '\0';
    return box_tag_payload(TAG_STRING, (uint64_t)(uintptr_t)hs);
}

Val val_bytes(Proc *p, const uint8_t *data, int len) {
    int total = sizeof(HeapBytes) + len;
    HeapBytes *hb = (HeapBytes *)proc_heap_alloc(p, total);
    hb->hdr.type = HEAP_BYTES;
    hb->hdr.flags = 0;
    hb->len = len;
    if (data && len > 0)
        memcpy(hb->data, data, len);
    return box_tag_payload(TAG_BYTES, (uint64_t)(uintptr_t)hb);
}

/* ============================================================
 * Value predicates & accessors
 *
 * val_int / val_from_double / val_get_int / val_get_float / val_to_double
 * live in ta_inline.h as static inline — they sit on the arithmetic
 * opcodes' hot path and must not be out-of-line calls (see the Value
 * helpers section there).
 * ============================================================ */

int val_is_nil(Val v) { return val_tag(v) == TAG_NIL; }

int val_is_pair(Val v) { return val_tag(v) == TAG_PAIR; }

int val_is_symbol(Val v) { return val_tag(v) == TAG_SYM; }
uint32_t val_get_symbol(Val v) { return val_payload32(v); }

int val_is_pid(Val v) { return val_tag(v) == TAG_PID; }
uint32_t val_get_pid(Val v) { return val_payload32(v); }

int val_is_clos(Val v) { return val_tag(v) == TAG_CLOS; }

int val_is_string(Val v) { return val_tag(v) == TAG_STRING; }
HeapString *val_get_string(Val v) { return (HeapString *)(uintptr_t)val_payload48(v); }

int val_is_bytes(Val v) { return val_tag(v) == TAG_BYTES; }
HeapBytes *val_get_bytes(Val v) { return (HeapBytes *)(uintptr_t)val_payload48(v); }

/* ============================================================
 * Deep copy — copy a value tree into a target process heap
 *
 * Immutability guarantees no cycles, so no visited table is needed. The
 * explicit work stacks are important: messages and captured values can be
 * much deeper than the native C stack.
 * ============================================================ */

typedef struct {
    int kind;
    Val value;
} CopyTask;

#define COPY_VALUE 0
#define COPY_PAIR 1
#define COPY_CLOSURE 2

static void copy_grow(void **items, int *cap, int count, size_t size) {
    if (count < *cap)
        return;
    int next = *cap ? *cap * 2 : 64;
    void *grown = realloc(*items, (size_t)next * size);
    if (!grown)
        abort();
    *items = grown;
    *cap = next;
}

Val val_deep_copy(Proc *target, Val root, int *res_err) {
    /* The worklist (tasks[]) and results[] keep Vals of the target heap in
     * malloc'd / C-local storage, off the TA stack, so no collection may run
     * while the tree is rebuilt: close the gate. Left un-drained on the way
     * out — the result is still unrooted; the caller roots it (the recv
     * builtins push it, or the next gate-open allocation) and the pending
     * request is honoured there. */
    proc_gc_enter(target);
    CopyTask *tasks = NULL;
    Val *results = NULL;
    int task_count = 0, task_cap = 0;
    int result_count = 0, result_cap = 0;
    /* spawn 路径（res_err 非 NULL）的收口校验表：本次拷贝造出的 header。
     * 既是 payload 去重表（`seen[i]->res` 比 `src->res`），也是拷贝成功
     * 后统一移交归属的名单。 */
    HeapResource **seen = NULL;
    int seen_count = 0, seen_cap = 0;
    Val result;

    copy_grow((void **)&tasks, &task_cap, task_count, sizeof(*tasks));
    tasks[task_count++] = (CopyTask){COPY_VALUE, root};

    while (task_count > 0) {
        CopyTask task = tasks[--task_count];
        uint16_t tag = val_tag(task.value);

        if (task.kind == COPY_PAIR) {
            Val cdr = results[--result_count];
            Val car = results[--result_count];
            copy_grow((void **)&results, &result_cap, result_count, sizeof(*results));
            results[result_count++] = val_pair(target, car, cdr);
            continue;
        }
        if (task.kind == COPY_CLOSURE) {
            HeapClosure *src = (HeapClosure *)(uintptr_t)val_payload48(task.value);
            int total = sizeof(HeapClosure) + (int)(src->nfree * sizeof(Val));
            HeapClosure *dst = (HeapClosure *)proc_heap_alloc(target, total);
            dst->hdr.type = HEAP_CLOS;
            dst->hdr.flags = 0;
            dst->entry = src->entry;
            dst->nfree = src->nfree;
            for (int i = src->nfree - 1; i >= 0; i--)
                dst->free[i] = results[--result_count];
            copy_grow((void **)&results, &result_cap, result_count, sizeof(*results));
            results[result_count++] = box_tag_payload(TAG_CLOS, (uint64_t)(uintptr_t)dst);
            continue;
        }

        if (val_is_float(task.value) || tag == TAG_INT || tag == TAG_NIL || tag == TAG_TRUE ||
            tag == TAG_FALSE || tag == TAG_PID || tag == TAG_SYM || tag == TAG_CLOS_ID ||
            tag == TAG_NATIVE) {
            copy_grow((void **)&results, &result_cap, result_count, sizeof(*results));
            results[result_count++] = task.value;
        } else if (tag == TAG_PAIR) {
            HeapPair *src = (HeapPair *)(uintptr_t)val_payload48(task.value);
            copy_grow((void **)&tasks, &task_cap, task_count, sizeof(*tasks));
            tasks[task_count++] = (CopyTask){COPY_PAIR, task.value};
            copy_grow((void **)&tasks, &task_cap, task_count, sizeof(*tasks));
            tasks[task_count++] = (CopyTask){COPY_VALUE, src->cdr};
            copy_grow((void **)&tasks, &task_cap, task_count, sizeof(*tasks));
            tasks[task_count++] = (CopyTask){COPY_VALUE, src->car};
        } else if (tag == TAG_STRING) {
            HeapString *src = (HeapString *)(uintptr_t)val_payload48(task.value);
            copy_grow((void **)&results, &result_cap, result_count, sizeof(*results));
            results[result_count++] = val_string(target, src->data, src->len);
        } else if (tag == TAG_BYTES) {
            HeapBytes *src = (HeapBytes *)(uintptr_t)val_payload48(task.value);
            copy_grow((void **)&results, &result_cap, result_count, sizeof(*results));
            results[result_count++] = val_bytes(target, src->data, src->len);
        } else if (tag == TAG_CLOS) {
            HeapClosure *src = (HeapClosure *)(uintptr_t)val_payload48(task.value);
            copy_grow((void **)&tasks, &task_cap, task_count, sizeof(*tasks));
            tasks[task_count++] = (CopyTask){COPY_CLOSURE, task.value};
            for (int i = src->nfree - 1; i >= 0; i--) {
                copy_grow((void **)&tasks, &task_cap, task_count, sizeof(*tasks));
                tasks[task_count++] = (CopyTask){COPY_VALUE, src->free[i]};
            }
        } else if (tag == TAG_RESOURCE) {
            HeapResource *src = (HeapResource *)(uintptr_t)val_payload48(task.value);
            if (res_err) {
                /* 收口校验（spawn 路径，docs/c-resource-design.md §4）：
                 * owner 必须是当前 proc（父），同一消息里的 payload 不重复
                 *（cons(r,r) 走 fork 等价路径，双保险）。检出即中止拷贝，
                 * 结果未定义 —— 调用方负责 proc_die。 */
                int bad = src->owner_pid != tls_current_proc->pid;
                for (int i = 0; !bad && i < seen_count; i++)
                    bad = src->res != NULL && seen[i]->res == src->res;
                if (bad) {
                    *res_err = 1;
                    result = val_nil();
                    goto copy_done;
                }
            }
            /* 拷贝 header。归属**先照抄**、挪到拷贝成功后统一改写（见
             * copy_done）：中途放弃的拷贝（上面的收口违规）会把半成品
             * header 留在 target 堆里，若此刻就已写成 target 的归属，收尾的
             * proc_die(target) 就会替发送方释放 payload——而发送方堆里的
             * 旧 header 还自称属主，下次释放即 UAF。照抄则失败路径的
             * 半成品仍归原属主，proc_die 扫到就跳过。
             * recv 路径（res_err == NULL）不需要改写：fragment 里的归属在
             * 投递时已写成接收方。 */
            HeapResource *dst = (HeapResource *)proc_heap_alloc(target, sizeof(HeapResource));
            dst->h.type = HEAP_RESOURCE;
            dst->h.flags = 0;
            dst->owner_pid = src->owner_pid;
            dst->res = src->res; /* payload 共享 */
            if (res_err) {
                copy_grow((void **)&seen, &seen_cap, seen_count, sizeof(*seen));
                seen[seen_count++] = dst;
            }
            copy_grow((void **)&results, &result_cap, result_count, sizeof(*results));
            results[result_count++] = box_tag_payload(TAG_RESOURCE, (uint64_t)(uintptr_t)dst);
        } else {
            fprintf(stderr, "val_deep_copy: unknown tag 0x%x\n", tag);
            abort(); /* 不许静默变 nil（docs/c-resource-design.md §8） */
        }
    }

    result = results[0];
copy_done:
    /* 移交归属（§4 三处改写之三的前半）：拷贝成功才把新 header 的 owner
     * 写成 target；父/发送方堆里的旧副本由投递路径（mbox_deliver /
     * LB_SPAWN）另行改写。失败则一个都不改（见上面的注释）。 */
    if (res_err && *res_err == 0)
        for (int i = 0; i < seen_count; i++)
            seen[i]->owner_pid = target->pid;
    free(tasks);
    free(results);
    free(seen);
    proc_gc_leave(target);
    return result;
}

/* Upper bound of the heap bytes val_deep_copy(target, v) will allocate.
 * DAG sharing is counted once per occurrence (an over-estimate, never
 * under). Callers use it to reserve arena room before a gate-closed copy:
 * the copy itself cannot grow the arena — its worklist holds unrooted
 * Vals — so the room must exist beforehand (proc_reserve_heap). */
typedef struct {
    Val value;
} SizeTask;

int val_calc_heap_size(Val root) {
    SizeTask *tasks = NULL;
    int count = 0, cap = 0;
    int total = 0;

    copy_grow((void **)&tasks, &cap, count, sizeof(*tasks));
    tasks[count++].value = root;
    while (count > 0) {
        Val v = tasks[--count].value;
        uint16_t tag = val_tag(v);
        if (tag == TAG_PAIR) {
            HeapPair *src = (HeapPair *)(uintptr_t)val_payload48(v);
            total += ta_heap_object_size(sizeof(HeapPair));
            copy_grow((void **)&tasks, &cap, count, sizeof(*tasks));
            tasks[count++].value = src->cdr;
            copy_grow((void **)&tasks, &cap, count, sizeof(*tasks));
            tasks[count++].value = src->car;
        } else if (tag == TAG_STRING) {
            HeapString *s = (HeapString *)(uintptr_t)val_payload48(v);
            total += ta_heap_object_size(sizeof(HeapString) + s->len + 1);
        } else if (tag == TAG_BYTES) {
            HeapBytes *b = (HeapBytes *)(uintptr_t)val_payload48(v);
            total += ta_heap_object_size(sizeof(HeapBytes) + b->len);
        } else if (tag == TAG_RESOURCE) {
            total += ta_heap_object_size(sizeof(HeapResource));
        } else if (tag == TAG_CLOS) {
            HeapClosure *c = (HeapClosure *)(uintptr_t)val_payload48(v);
            total += ta_heap_object_size(sizeof(HeapClosure) + c->nfree * (int)sizeof(Val));
            for (int i = c->nfree - 1; i >= 0; i--) {
                copy_grow((void **)&tasks, &cap, count, sizeof(*tasks));
                tasks[count++].value = c->free[i];
            }
        }
    }
    free(tasks);
    return total;
}

/* Deep copy for callback convergence (issue #160): like val_deep_copy, but
 * the three-way dispatch is the point — chunk objects rebuild in the target
 * heap, arena objects pass through untouched (shared structure stays
 * shared), immediates as-is. The caller has set the arena's converge flag so
 * the copy's own proc_heap_alloc calls go to the heap, and closed the gate
 * so no collection runs while the worklist holds unrooted Vals.
 * Immutability → no cycles. DAG sharing is preserved with forwarding
 * pointers written into the chunk objects (the arena is freed right after,
 * so markers need no cleanup) — each chunk object rebuilds exactly once,
 * keeping allocation at the caller's a->total bound and identity
 * stable, same as the slow GC path. */
Val val_converge_copy(Proc *target, Val root) {
    CopyTask *tasks = NULL;
    Val *results = NULL;
    int task_count = 0, task_cap = 0;
    int result_count = 0, result_cap = 0;

    copy_grow((void **)&tasks, &task_cap, task_count, sizeof(*tasks));
    tasks[task_count++] = (CopyTask){COPY_VALUE, root};

    while (task_count > 0) {
        CopyTask task = tasks[--task_count];
        uint16_t tag = val_tag(task.value);

        if (task.kind == COPY_PAIR) {
            Val cdr = results[--result_count];
            Val car = results[--result_count];
            Val nv = val_pair(target, car, cdr);
            /* Leave a forwarding pointer so later occurrences of this pair
             * (DAG sharing) reuse the copy instead of duplicating it. */
            HeapHeader *h = (HeapHeader *)(uintptr_t)val_payload48(task.value);
            h->flags |= FLAG_FORWARDED;
            void *fwd = (void *)(uintptr_t)val_payload48(nv);
            memcpy((uint8_t *)h + sizeof(HeapHeader), &fwd, sizeof(void *));
            copy_grow((void **)&results, &result_cap, result_count, sizeof(*results));
            results[result_count++] = nv;
            continue;
        }
        if (task.kind == COPY_CLOSURE) {
            HeapClosure *src = (HeapClosure *)(uintptr_t)val_payload48(task.value);
            int total = sizeof(HeapClosure) + (int)(src->nfree * sizeof(Val));
            HeapClosure *dst = (HeapClosure *)proc_heap_alloc(target, total);
            dst->hdr.type = HEAP_CLOS;
            dst->hdr.flags = 0;
            dst->entry = src->entry;
            dst->nfree = src->nfree;
            for (int i = src->nfree - 1; i >= 0; i--)
                dst->free[i] = results[--result_count];
            HeapHeader *h = (HeapHeader *)src;
            h->flags |= FLAG_FORWARDED;
            void *fwd = (void *)(uintptr_t)dst;
            memcpy((uint8_t *)h + sizeof(HeapHeader), &fwd, sizeof(void *));
            copy_grow((void **)&results, &result_cap, result_count, sizeof(*results));
            results[result_count++] = box_tag_payload(TAG_CLOS, (uint64_t)(uintptr_t)dst);
            continue;
        }

        if (val_is_float(task.value) || tag == TAG_INT || tag == TAG_NIL || tag == TAG_TRUE ||
            tag == TAG_FALSE || tag == TAG_PID || tag == TAG_SYM || tag == TAG_CLOS_ID ||
            tag == TAG_NATIVE) {
            copy_grow((void **)&results, &result_cap, result_count, sizeof(*results));
            results[result_count++] = task.value;
            continue;
        }

        void *payload = (void *)(uintptr_t)val_payload48(task.value);
        if (!val_in_chunk(target, payload)) {
            /* Arena object: already in the target heap, keep the reference. */
            copy_grow((void **)&results, &result_cap, result_count, sizeof(*results));
            results[result_count++] = task.value;
        } else if (((HeapHeader *)payload)->flags & FLAG_FORWARDED) {
            /* Shared chunk substructure: reuse the copy made for the first
             * occurrence instead of duplicating it — identity and the
             * caller's arena total (CallbackArena.total) depend on
             * rebuilding each object exactly once. */
            void *fwd;
            memcpy(&fwd, (uint8_t *)payload + sizeof(HeapHeader), sizeof(void *));
            copy_grow((void **)&results, &result_cap, result_count, sizeof(*results));
            results[result_count++] = box_tag_payload(tag, (uint64_t)(uintptr_t)fwd);
        } else if (tag == TAG_PAIR) {
            HeapPair *src = (HeapPair *)payload;
            copy_grow((void **)&tasks, &task_cap, task_count, sizeof(*tasks));
            tasks[task_count++] = (CopyTask){COPY_PAIR, task.value};
            copy_grow((void **)&tasks, &task_cap, task_count, sizeof(*tasks));
            tasks[task_count++] = (CopyTask){COPY_VALUE, src->cdr};
            copy_grow((void **)&tasks, &task_cap, task_count, sizeof(*tasks));
            tasks[task_count++] = (CopyTask){COPY_VALUE, src->car};
        } else if (tag == TAG_STRING) {
            HeapString *src = (HeapString *)payload;
            Val nv = val_string(target, src->data, src->len);
            ((HeapHeader *)payload)->flags |= FLAG_FORWARDED;
            void *fwd = (void *)(uintptr_t)val_payload48(nv);
            memcpy((uint8_t *)payload + sizeof(HeapHeader), &fwd, sizeof(void *));
            copy_grow((void **)&results, &result_cap, result_count, sizeof(*results));
            results[result_count++] = nv;
        } else if (tag == TAG_BYTES) {
            HeapBytes *src = (HeapBytes *)payload;
            Val nv = val_bytes(target, src->data, src->len);
            ((HeapHeader *)payload)->flags |= FLAG_FORWARDED;
            void *fwd = (void *)(uintptr_t)val_payload48(nv);
            memcpy((uint8_t *)payload + sizeof(HeapHeader), &fwd, sizeof(void *));
            copy_grow((void **)&results, &result_cap, result_count, sizeof(*results));
            results[result_count++] = nv;
        } else if (tag == TAG_CLOS) {
            HeapClosure *src = (HeapClosure *)payload;
            copy_grow((void **)&tasks, &task_cap, task_count, sizeof(*tasks));
            tasks[task_count++] = (CopyTask){COPY_CLOSURE, task.value};
            for (int i = src->nfree - 1; i >= 0; i--) {
                copy_grow((void **)&tasks, &task_cap, task_count, sizeof(*tasks));
                tasks[task_count++] = (CopyTask){COPY_VALUE, src->free[i]};
            }
        } else if (tag == TAG_RESOURCE) {
            /* cfunc 在 ccall 窗口里造的资源 header 落在 chunk，回调收敛时
             * rebuild 进堆（payload 不动，共享）；forwarding 同 string。 */
            HeapResource *src = (HeapResource *)payload;
            HeapResource *dst = (HeapResource *)proc_heap_alloc(target, sizeof(HeapResource));
            dst->h.type = HEAP_RESOURCE;
            dst->h.flags = 0;
            dst->owner_pid = src->owner_pid;
            dst->res = src->res;
            ((HeapHeader *)payload)->flags |= FLAG_FORWARDED;
            void *fwd = (void *)(uintptr_t)dst;
            memcpy((uint8_t *)payload + sizeof(HeapHeader), &fwd, sizeof(void *));
            copy_grow((void **)&results, &result_cap, result_count, sizeof(*results));
            results[result_count++] = box_tag_payload(TAG_RESOURCE, (uint64_t)(uintptr_t)dst);
        } else {
            fprintf(stderr, "val_converge_copy: unknown tag 0x%x\n", tag);
            abort(); /* 不许静默变 nil（docs/c-resource-design.md §8） */
        }
    }

    Val result = results[0];
    free(tasks);
    free(results);
    return result;
}

/* ================================================================
 * print_val — 可打印值 → stdout（print 内建 / 进程退出值 / trace 共用）
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
        const char *name = vm_symbol_name(vm, val_get_symbol(v));
        printf("%s", name ? name : "?");
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
    } else if (val_get_resource(v) != NULL) {
        printf("<resource>"); /* 不透明：不吐地址不吐内部状态（§7.4） */
    } else {
        printf("?");
    }
}

/* Equality for OP_EQ/OP_NE（LOP_EQ/LOP_JIF_EQ 的非数值路径）。Strings
 * compare by content (HeapString holds len + NUL-terminated data);
 * ints/symbols/nil/true/false compare by their NaN-boxed value (int payload
 * is direct, symbols are interned); everything else (pair/closure/bytes/pid)
 * compares by pointer identity. */
int val_equal(Val a, Val b) {
    if (val_is_string(a) && val_is_string(b)) {
        HeapString *sa = val_get_string(a);
        HeapString *sb = val_get_string(b);
        return sa->len == sb->len && memcmp(sa->data, sb->data, sa->len) == 0;
    }
    return a == b;
}
