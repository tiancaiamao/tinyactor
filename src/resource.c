/*
 * resource.c — C 模块不透明资源值（docs/c-resource-design.md，issue #401）
 *
 * 形态：每持有者一份堆内 HeapResource header（GC 搬运、按引用透传）+
 * 堆外共享 ResourcePayload（released 的唯一所在地，永不 free）。
 *
 * 本文件只放资源专属的逻辑（构造、释放、树遍历、投递校验、空间扫描）；
 * 归属移交发生在投递收口 mbox_deliver（scheduler.c），GC/死亡/teardown
 * 兜底的调用点分别在 gc.c / scheduler.c / api.c。
 *
 * 约定：
 * - TAG_RESOURCE 的载荷就是 HeapResource*（指针本身 < 2^48，天然满足
 *   NaN-boxing 的 48 位载荷约定，同 TAG_CLOS_ID/TAG_NATIVE 的公开读写
 *   约定，见 ta.h 标签表注释）；
 * - 所有树遍历只下钻 pair 与 closure —— 其余 tag 要么是立即数要么无子
 *   引用；fragment 与堆内对象布局同构，同一套遍历两边通用；
 * - dtor 的执行上下文约束见 docs/c-module.md §6.1（不分配 TA 值、不重入
 *   VM、可以阻塞）；执行者是属主线程或 join 后的单线程。
 */

#include "ta.h"
#include <stdio.h>
#include <stdlib.h>

/* 载荷读写按 ta.h 标签表的公开约定（高 16 tag / 低 48 payload），与 main.c
 * 装载器读 TAG_NATIVE 同款 —— val.c 的 val_payload48/box_tag_payload 未导出。 */
static inline Val box_resource(HeapResource *r) {
    uint64_t bits = (uint64_t)(uintptr_t)r;
    return (Val)((uint64_t)TAG_RESOURCE << 48 | bits);
}

HeapResource *val_get_resource(Val v) {
    if (val_tag(v) != TAG_RESOURCE)
        return NULL;
    return (HeapResource *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
}

Val ta_resource_new(Proc *p, void *ptr, void (*dtor)(void *)) {
    /* ccall 窗口内创建时 header 落 chunk arena、退出时 val_converge_copy
     * rebuild 进堆；payload 计数留给 do_ccall 收口查（yield / cfunc_die
     * 丢链即泄漏）。见 ta.h CallbackArena.resources。 */
    if (p->ccall != NULL)
        p->ccall->resources++;
    ResourcePayload *res = malloc(sizeof(ResourcePayload));
    if (!res)
        abort(); /* 与 val_pair 等构造子同款：OOM 直接 abort */
    res->ptr = ptr;
    res->dtor = dtor;
    res->released = 0;
    HeapResource *r = (HeapResource *)proc_heap_alloc(p, sizeof(HeapResource));
    r->h.type = HEAP_RESOURCE;
    r->h.flags = 0;
    r->owner_pid = p->pid;
    r->res = res;
    return box_resource(r);
}

void ta_resource_release(HeapResource *r) {
    ResourcePayload *p = r->res;
    if (p->released)
        return; /* 副本已释放过：released 在共享 payload 上，天然幂等 */
    p->released = 1;
    if (p->ptr && p->dtor)
        p->dtor(p->ptr);
    p->ptr = NULL;
}

/* ================================================================
 * 树遍历：收集树里全部 HEAP_RESOURCE header
 *
 * 显式工作表递归（pair 下钻 car/cdr、closure 下钻 free[]），不走 C 递归
 * —— 消息树的深度无上界。工作表与结果数组 malloc，调用方负责 free。
 * collect_resources 是下面全部操作的公共走法；每消息资源数很小，重复
 * 收集（校验一遍、改写一遍）换来的是每个函数各自独立可读。
 * ================================================================ */

static HeapResource **collect_resources(Val root, int *count_out) {
    HeapResource **found = NULL;
    int n = 0, cap = 0;
    Val *work = malloc(sizeof(Val));
    int wn = 0, wcap = 1;
    if (!work)
        abort();
    work[wn++] = root;

    while (wn > 0) {
        Val v = work[--wn];
        uint16_t tag = val_tag(v);
        if (tag == TAG_PAIR) {
            HeapPair *pr = (HeapPair *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
            if (wn + 2 > wcap) {
                wcap *= 2;
                work = realloc(work, (size_t)wcap * sizeof(Val));
                if (!work)
                    abort();
            }
            work[wn++] = pr->cdr;
            work[wn++] = pr->car;
        } else if (tag == TAG_CLOS) {
            HeapClosure *cl = (HeapClosure *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
            if (wn + cl->nfree > wcap) {
                while (wn + cl->nfree > wcap)
                    wcap *= 2;
                work = realloc(work, (size_t)wcap * sizeof(Val));
                if (!work)
                    abort();
            }
            for (int i = 0; i < cl->nfree; i++)
                work[wn++] = cl->free[i];
        } else if (tag == TAG_RESOURCE) {
            if (n == cap) {
                cap = cap ? cap * 2 : 8;
                found = realloc(found, (size_t)cap * sizeof(HeapResource *));
                if (!found)
                    abort();
            }
            found[n++] = (HeapResource *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
        }
    }
    free(work);
    *count_out = n;
    return found;
}

int val_has_resource(Val root) {
    int n = 0;
    HeapResource **found = collect_resources(root, &n);
    free(found);
    return n > 0;
}

void val_set_resource_owner(Val root, int owner_pid) {
    int n = 0;
    HeapResource **found = collect_resources(root, &n);
    for (int i = 0; i < n; i++)
        found[i]->owner_pid = owner_pid;
    free(found);
}

void resource_release_tree(Val root, int owner_pid) {
    int n = 0;
    HeapResource **found = collect_resources(root, &n);
    for (int i = 0; i < n; i++)
        if (found[i]->owner_pid == owner_pid)
            ta_resource_release(found[i]);
    free(found);
}

int resource_send_check(Val msg, Proc *sender, Proc *target) {
    int n = 0;
    HeapResource **found = collect_resources(msg, &n);
    int ok = 0;
    if (n == 0)
        goto out;
    /* poller 线程（timer fire）上 tls_current_proc 为 NULL —— 资源分支虽
     * 因 timer_register 已拒而不可达，校验仍 NULL-safe（R2 N3）：无属主
     * 上下文一律判违规。 */
    if (!sender || target == sender)
        goto out;
    for (int i = 0; i < n; i++) {
        if (found[i]->owner_pid != sender->pid)
            goto out; /* 非属主发送 / 二次 send（fan-out 循环发送同一资源） */
        for (int j = 0; j < i; j++)
            if (found[j]->res == found[i]->res)
                goto out; /* 同一消息里重复 payload（cons(r,r)），双保险之一 */
    }
    ok = 1;
out:
    free(found);
    return ok ? 0 : -1;
}

/* ================================================================
 * resource_release_space — 线性扫一段 bump 堆，兜底释放垃圾资源
 *
 * [space, space+bytes) 是 bump 分配器的一段连续空间（无洞、8 对齐），
 * 按 obj_size 步进走一遍：HEAP_RESOURCE 且未转发且 owner == pid 的跑
 * resource_release；其余对象只步进，不读子引用（这些对象是垃圾或已
 * 搬走的旧副本，逐个下钻既无必要也违反半空间的只写 tospace 约定）。
 *
 * 两个调用形态（skip_forwarded）：
 * - GC 的 fromspace 兜底（gc.c，skip_forwarded=1）：未转发的活资源是
 *   确认不可达的垃圾，转发指针已写过的副本跳过（见下）；
 * - proc_die / vm_free 的活堆兜底（scheduler.c / api.c，0）：死进程的
 *   活堆，全堆都是垃圾，没有转发标记。
 *
 * forwarding 指针写在 obj+sizeof(HeapHeader) 起 8 字节（gc.c gc_copy_obj）：
 * 对 HEAP_RESOURCE 覆盖 padding + owner_pid + res 低 2 字节，对
 * HEAP_STRING / HEAP_CLOS 覆盖 len / nfree —— 被转发的副本只看
 * FLAG_FORWARDED，字段不再被读（含 res）；**步进尺寸除外**：字段被踩就
 * 无法从原地算尺寸，改从 tospace 副本读（memcpy 发生在踩踏之前，副本
 * 字段完整，与分配时同一份公式，尺寸必然一致）。
 * ================================================================ */

void resource_release_space(uint8_t *space, int bytes, int pid, int skip_forwarded) {
    int off = 0;
    while (off < bytes) {
        HeapHeader *h = (HeapHeader *)(space + off);
        int forwarded = skip_forwarded && (h->flags & FLAG_FORWARDED);
        if (h->type == HEAP_RESOURCE && !forwarded) {
            HeapResource *r = (HeapResource *)h;
            if (r->owner_pid == pid)
                ta_resource_release(r);
        }
        int sz;
        if (forwarded) {
            /* 字段被 forwarding 踩坏，尺寸从 tospace 副本读（见上） */
            void *fwd;
            memcpy(&fwd, (uint8_t *)h + sizeof(HeapHeader), sizeof(void *));
            sz = gc_obj_size(fwd);
        } else {
            sz = gc_obj_size(h);
        }
        off += (sz + 7) & ~7; /* 与 gc_scan_tospace 同款 8 对齐步进 */
    }
}