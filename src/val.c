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

/* val_nil / val_true / val_false are static inline in ta_inline.h (constants
 * on the exit path of both dispatchers; see the note there). */

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

/* Slots left uninitialized on purpose: every caller fills them in after the
 * allocation (op_tuple from the operand stack, the copies from `results`),
 * and a fill-before-allocate would move the source under GC. */
Val val_tuple_new(Proc *p, int n) {
    int total = sizeof(HeapTuple) + n * (int)sizeof(Val);
    HeapTuple *ht = (HeapTuple *)proc_heap_alloc(p, total);
    ht->hdr.type = HEAP_TUPLE;
    ht->hdr.flags = 0;
    ht->len = n;
    return box_tag_payload(TAG_TUPLE, (uint64_t)(uintptr_t)ht);
}

/* DOWN protocol (lib/down.ta) — (down.Down ref pid reason), where reason is
 * an ExitReason. The VM holds crash reasons as interned symbols (language
 * machinery); only the named cases get their own variant, everything else
 * crosses into TA as Other(String). Constructor tags carry the canonical
 * module prefix (`down.`), exactly as imported ADT constructors do in TA.
 * Build needs the byte count reserved and the GC gate closed by the caller. */
int down_msg_heap_bytes(VM *vm, Val reason) {
    int bytes = ta_heap_object_size(sizeof(HeapTuple) + 4 * (int)sizeof(Val));
    if (!val_is_nil(reason)) {
        int noproc = vm_intern_symbol(vm, "noproc");
        if (!val_is_symbol(reason) || val_get_symbol(reason) != (uint32_t)noproc) {
            const char *name =
                val_is_symbol(reason) ? vm_symbol_name(vm, val_get_symbol(reason)) : "unknown";
            bytes += ta_heap_object_size(sizeof(HeapTuple) + 2 * (int)sizeof(Val));
            bytes += ta_heap_object_size(sizeof(HeapString) + (int)strlen(name) + 1);
        }
    }
    return bytes;
}

Val down_msg_build(VM *vm, Proc *p, Val ref, Val pid, Val reason) {
    int down_sym = vm_intern_symbol(vm, "down.Down");
    int normal_sym = vm_intern_symbol(vm, "down.Normal");
    int noproc_sym = vm_intern_symbol(vm, "down.Noproc");
    int other_sym = vm_intern_symbol(vm, "down.Other");

    Val exit_reason;
    if (val_is_nil(reason)) {
        exit_reason = val_symbol((uint32_t)normal_sym);
    } else if (val_is_symbol(reason) &&
               val_get_symbol(reason) == (uint32_t)vm_intern_symbol(vm, "noproc")) {
        exit_reason = val_symbol((uint32_t)noproc_sym);
    } else {
        const char *name =
            val_is_symbol(reason) ? vm_symbol_name(vm, val_get_symbol(reason)) : "unknown";
        Val text = val_string(p, name, (int)strlen(name));
        Val other = val_tuple_new(p, 2);
        HeapTuple *ot = val_as_tuple(other);
        ot->slot[0] = val_symbol((uint32_t)other_sym);
        ot->slot[1] = text;
        exit_reason = other;
    }

    Val msg = val_tuple_new(p, 4);
    HeapTuple *ht = val_as_tuple(msg);
    ht->slot[0] = val_symbol((uint32_t)down_sym);
    ht->slot[1] = ref;
    ht->slot[2] = pid;
    ht->slot[3] = exit_reason;
    return msg;
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

/* val_is_pair is static inline in ta_inline.h (OP_CAR/OP_CDR gate). */

int val_is_symbol(Val v) { return val_tag(v) == TAG_SYM; }
uint32_t val_get_symbol(Val v) { return val_payload32(v); }

int val_is_pid(Val v) { return val_tag(v) == TAG_PID; }
uint32_t val_get_pid(Val v) { return val_payload32(v); }

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

#define COPY_VALUE 0 /* 求值：拷贝 task.value 这个子值 */
#define COPY_NODE 1  /* 装配：task.value 是个孩子都拷好了的容器 */

/* 把副本地址写进 chunk 对象的 forwarding 槽（与 GC 同一个套路：紧跟
 * HeapHeader 的那 8 字节）。arena 拷完整块释放，标记不用清理。
 * 闭包不能这么转发 —— 那 8 字节对它是 entry/nfree，分配时也没给槽留位置。 */
static void chunk_forward(void *payload, Val copy) {
    ((HeapHeader *)payload)->flags |= FLAG_FORWARDED;
    void *fwd = val_heap_ptr(copy);
    memcpy((uint8_t *)payload + sizeof(HeapHeader), &fwd, sizeof(void *));
}

/* 装配阶段：孩子已按构造顺序压在 results 上，倒序弹回填进 target 堆里的新
 * 骨架。深拷与收敛拷贝共用一份 —— 两者对「值从哪来、拷完要不要转发」各有
 * 主张，但把 pair/tuple/closure 装起来是同一件事（漏填一个字段就是内存
 * 垃圾，这种代码不该有两份）。 */
static Val copy_attach(Proc *target, Val v, Val *results, int *result_count) {
    uint16_t tag = val_tag(v);
    if (tag == TAG_PAIR) {
        Val cdr = results[--*result_count];
        Val car = results[--*result_count];
        return val_pair(target, car, cdr);
    }
    if (tag == TAG_TUPLE) {
        int n;
        val_node_shape(NULL, v, &n, NULL);
        Val nv = val_tuple_new(target, n);
        HeapTuple *dst = val_as_tuple(nv);
        for (int i = n - 1; i >= 0; i--)
            dst->slot[i] = results[--*result_count];
        return nv;
    }
    HeapClosure *src = val_as_clos(v);
    int n = src->nfree;
    HeapClosure *dst =
        (HeapClosure *)proc_heap_alloc(target, sizeof(HeapClosure) + (int)(n * sizeof(Val)));
    dst->hdr.type = HEAP_CLOS;
    dst->hdr.flags = 0;
    dst->entry = src->entry;
    dst->nfree = n;
    for (int i = n - 1; i >= 0; i--)
        dst->free[i] = results[--*result_count];
    return box_tag_payload(TAG_CLOS, (uint64_t)(uintptr_t)dst);
}

/* 原子堆对象（string / bytes / resource）：正文拷进 target 堆，resource 的
 * payload 指针照抄（堆外，共享）——归属改写不在这里做，两条路径各有收口
 *（深拷等拷贝成功才改，收敛保持原样）。pair/tuple/closure 有孩子，走不到
 * 这里（val_node_shape 已按 tag 分派过）。 */
static Val atom_rebuild(Proc *target, Val v) {
    void *payload = val_heap_ptr(v);
    switch (val_tag(v)) {
    case TAG_STRING: {
        HeapString *src = payload;
        return val_string(target, src->data, src->len);
    }
    case TAG_BYTES: {
        HeapBytes *src = payload;
        return val_bytes(target, src->data, src->len);
    }
    case TAG_RESOURCE: {
        HeapResource *src = payload;
        HeapResource *dst = (HeapResource *)proc_heap_alloc(target, sizeof(HeapResource));
        dst->h.type = HEAP_RESOURCE;
        dst->h.flags = 0;
        dst->owner_pid = src->owner_pid;
        dst->res = src->res; /* payload 共享 */
        return box_tag_payload(TAG_RESOURCE, (uint64_t)(uintptr_t)dst);
    }
    default:
        fprintf(stderr, "val copy: unknown heap atom tag 0x%x\n", val_tag(v));
        abort(); /* 不许静默变 nil（docs/c-resource-design.md §8） */
    }
}

Val val_deep_copy(Proc *self, Proc *target, Val root, int *res_err) {
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

    TA_PUSH(tasks, task_count, task_cap, ((CopyTask){COPY_VALUE, root}));

    while (task_count > 0) {
        CopyTask task = tasks[--task_count];
        Val v = task.value;
        uint16_t tag = val_tag(v);

        if (task.kind == COPY_NODE) {
            TA_PUSH(results, result_count, result_cap,
                    copy_attach(target, v, results, &result_count));
            continue;
        }

        int nk;
        const Val *kids = val_node_shape(NULL, v, &nk, NULL);
        if (kids) {
            /* 先压装配任务，再倒序压孩子：孩子全拷完才轮到装配。 */
            TA_PUSH(tasks, task_count, task_cap, ((CopyTask){COPY_NODE, v}));
            for (int i = nk - 1; i >= 0; i--)
                TA_PUSH(tasks, task_count, task_cap, ((CopyTask){COPY_VALUE, kids[i]}));
            continue;
        }
        if (!val_is_heap(v)) { /* 立即数 */
            TA_PUSH(results, result_count, result_cap, v);
            continue;
        }
        if (tag == TAG_RESOURCE && res_err) {
            /* 收口校验（spawn 路径，docs/c-resource-design.md §4）：owner
             * 必须是当前 proc（父），同一消息里的 payload 不重复
             *（cons(r,r) 走 fork 等价路径，双保险）。检出即中止拷贝，
             * 结果未定义 —— 调用方负责 proc_die。 */
            HeapResource *src = val_heap_ptr(v);
            int bad = src->owner_pid != self->pid;
            for (int i = 0; !bad && i < seen_count; i++)
                bad = src->res != NULL && seen[i]->res == src->res;
            if (bad) {
                *res_err = 1;
                result = val_nil();
                goto copy_done;
            }
        }
        /* atom_rebuild 把 header 的归属**照抄**过来，挪到拷贝成功后统一改写
         * （见 copy_done）：中途放弃的拷贝（上面的收口违规）会把半成品
         * header 留在 target 堆里，若此刻就已写成 target 的归属，收尾的
         * proc_die(target) 就会替发送方释放 payload——而发送方堆里的旧
         * header 还自称属主，下次释放即 UAF。照抄则失败路径的半成品仍归
         * 原属主，proc_die 扫到就跳过。
         * recv 路径（res_err == NULL）不需要改写：fragment 里的归属在投递
         * 时已写成接收方。 */
        Val nv = atom_rebuild(target, v);
        if (res_err && tag == TAG_RESOURCE)
            TA_PUSH(seen, seen_count, seen_cap, (HeapResource *)val_heap_ptr(nv));
        TA_PUSH(results, result_count, result_cap, nv);
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

/* 值树的总字节数，每个对象 8 字节对齐。两种调用方只差一个数：
 *
 *   min_object — 最小对象下限。GC 堆要给转发指针留槽（ta_heap_object_size），
 *                所以裸尺寸很小的对象（空 tuple / 空 bytes 都是 8 字节）也占
 *                TA_HEAP_MIN_OBJECT（16）。消息分片不是 GC 堆、不需要那个槽，
 *                两个分片调用方（scheduler.c 投递 / lib/timer.c 快照）传 0。
 *   has_res   — 非 NULL 时顺带报「树里含资源」（投递前的快路径校验）。
 *
 * 用途是「关 GC 门之前预留空间」：val_deep_copy 的工作表里放的是尚未
 * root 的 Val，拷贝期间不能再触发收集，所以位置必须先留出来。DAG 共享按
 * 出现次数重复计数 —— 只会高估、不会低估。 */
int val_tree_size(Val root, int min_object, int *has_res) {
    Val *work = NULL;
    int n = 0, cap = 0, total = 0;

    if (has_res)
        *has_res = 0;
    TA_PUSH(work, n, cap, root);
    while (n > 0) {
        Val v = work[--n];
        int nk, raw;
        const Val *kids = val_node_shape(NULL, v, &nk, &raw);
        int size = (raw + 7) & ~7;
        total += size < min_object ? min_object : size;
        if (has_res && val_tag(v) == TAG_RESOURCE)
            *has_res = 1;
        for (int i = nk - 1; i >= 0; i--)
            TA_PUSH(work, n, cap, kids[i]);
    }
    free(work);
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

    TA_PUSH(tasks, task_count, task_cap, ((CopyTask){COPY_VALUE, root}));

    while (task_count > 0) {
        CopyTask task = tasks[--task_count];
        Val v = task.value;
        uint16_t tag = val_tag(v);

        if (task.kind == COPY_NODE) {
            Val nv = copy_attach(target, v, results, &result_count);
            /* DAG 共享：留下 forwarding，后出现的同一对象复用这份副本，让
             * 每个对象只重建一次（分配量守住调用方的 arena 上限，身份也
             * 稳定）。闭包不能转发，见 chunk_forward 的注释。 */
            if (tag == TAG_PAIR || tag == TAG_TUPLE)
                chunk_forward(val_heap_ptr(v), nv);
            TA_PUSH(results, result_count, result_cap, nv);
            continue;
        }

        if (!val_is_heap(v)) { /* 立即数原样 */
            TA_PUSH(results, result_count, result_cap, v);
            continue;
        }
        void *payload = val_heap_ptr(v);
        if (!val_in_chunk(target, payload)) {
            /* Arena object: already in the target heap, keep the reference. */
            TA_PUSH(results, result_count, result_cap, v);
            continue;
        }
        if (((HeapHeader *)payload)->flags & FLAG_FORWARDED) {
            /* Shared chunk substructure: reuse the copy made for the first
             * occurrence instead of duplicating it — identity and the
             * caller's arena total (CallbackArena.total) depend on
             * rebuilding each object exactly once. */
            void *fwd;
            memcpy(&fwd, (uint8_t *)payload + sizeof(HeapHeader), sizeof(void *));
            TA_PUSH(results, result_count, result_cap,
                    box_tag_payload(tag, (uint64_t)(uintptr_t)fwd));
            continue;
        }
        int nk;
        const Val *kids = val_node_shape(NULL, v, &nk, NULL);
        if (kids) {
            TA_PUSH(tasks, task_count, task_cap, ((CopyTask){COPY_NODE, v}));
            for (int i = nk - 1; i >= 0; i--)
                TA_PUSH(tasks, task_count, task_cap, ((CopyTask){COPY_VALUE, kids[i]}));
            continue;
        }
        /* 原子堆对象：string / bytes / resource，拷进堆并转发。cfunc 在 ccall
         * 窗口里造的资源 header 也落在 chunk，回调收敛时同样 rebuild 进堆
         *（payload 不动，共享）。 */
        Val nv = atom_rebuild(target, v);
        chunk_forward(payload, nv);
        TA_PUSH(results, result_count, result_cap, nv);
    }

    Val result = results[0];
    free(tasks);
    free(results);
    return result;
}

/* ================================================================
 * print_val — 可打印值 → stdout（print 内建 / 进程退出值）
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
    } else if (val_is_tuple(v)) {
        /* `(tuple a b)`：与 .kl / AST 的拼法同一个形状（tuple 在文本层就是
         * 一个 `(tuple …)` 调用），所以打印出来的东西既是人能读的，也是
         * 这个仓库里唯一的 tuple 记法。与 `(list 'tuple a b)` 的打印结果相同
         * —— 值打印不是机器可读回的格式，这一个歧义换来的是“一个形状一个
         * 说法”。 */
        HeapTuple *t = val_as_tuple(v);
        printf("(tuple");
        for (int i = 0; i < t->len; i++) {
            printf(" ");
            print_val(vm, t->slot[i]);
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

/* val_equal is static inline in ta_inline.h: it fires on every OP_EQ/OP_NE
 * (its fast path is the `a == b` compare, with the string memcmp behind the
 * tag test). Kept out-of-line here it cost a bl/ret per comparison. */
