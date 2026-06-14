# Task: Phase 4 Task 2 — Heap Fragment 消息传递

## Context

TinyActor Phase 4 Task 1 已完成：线程基础设施就位（runq mutex, tls_current_proc, atomic pid），但 mailbox 仍是 Val 数组，send 仍在目标堆上 val_deep_copy——多线程下这会导致 GC 竞态。

本任务将 mailbox 从 Val 数组改为 MsgFragment 链表。send 在 malloc'd fragment 上拷贝消息（不碰目标堆），recv 时从 fragment 反序列化到自己的堆。

**GC 零改动是核心约束。**

## Current State (what to change)

### ta.h — Proc 的 mailbox 字段

当前（line ~128）：
```c
Val      *mbox;
int       mbox_head, mbox_tail, mbox_count, mbox_cap;
pthread_mutex_t mbox_lock;
```

改为：
```c
MsgFragment *mbox_head;       // 消息链表头
MsgFragment *mbox_tail;       // 消息链表尾
int          mbox_count;      // 消息数
pthread_mutex_t mbox_lock;
```

### vm.c — 需要修改的函数

**proc_new (line ~169)**：删除 mbox 数组初始化，改为：
```c
p->mbox_head   = NULL;
p->mbox_tail   = NULL;
p->mbox_count  = 0;
```

**mbox_push (line ~85)**：完全重写。

旧版直接写 Val 数组。新版：
1. 计算 msg 需要的 fragment 大小
2. malloc MsgFragment
3. 在 fragment 内拷贝 msg（保留堆对象布局）
4. 锁目标 mbox_lock
5. 挂入链表尾部
6. 解锁

**mbox_pop (line ~95)**：完全重写。

旧版从 Val 数组读。新版：
1. 锁自己的 mbox_lock（执行期间无竞争，但 fragment 是共享结构）
2. 取链表头 fragment
3. 解锁
4. val_deep_copy(p, frag->root) 到自己的堆
5. free(frag)

**OP_SEND (line ~716)**：改为用新的 fragment-based mbox_push。

旧版：`mbox_push(t, val_deep_copy(t, msg));`
新版：`mbox_push(t, msg);` — fragment 拷贝在 mbox_push 内部完成

**OP_RECV (line ~726)**：改为用新的 fragment-based mbox_pop。

旧版：`proc_push(p, mbox_pop(p));`
新版：`proc_push(p, mbox_pop(p));` — mbox_pop 内部做 deep_copy

**proc_die (line ~206)**：monitor 的 DOWN 消息改用 fragment。

旧版直接在 watcher 堆上构建 pair 链。新版用 mbox_push 发 DOWN 消息。

**vm_free / proc_free**：释放 mailbox 中的所有 fragment。

## New Functions to Implement

### frag_calc_size — 计算 Val 树的 fragment 空间需求

```c
// 返回 Val 树在 fragment data[] 中需要的字节数（不含 Val 本身）
static int frag_calc_size(Val v) {
    uint16_t tag = val_tag(v);
    if (tag == TAG_PAIR) {
        HeapPair *src = (HeapPair *)(uintptr_t)val_payload48(v);
        return sizeof(HeapPair) + frag_calc_size(src->car) + frag_calc_size(src->cdr);
    }
    if (tag == TAG_STRING) {
        HeapString *s = (HeapString *)(uintptr_t)val_payload48(v);
        return sizeof(HeapString) + s->len + 1;
    }
    if (tag == TAG_CLOS) {
        HeapClosure *c = (HeapClosure *)(uintptr_t)val_payload48(v);
        int sz = sizeof(HeapClosure) + c->nfree * sizeof(Val);
        for (int i = 0; i < c->nfree; i++)
            sz += frag_calc_size(c->free[i]);
        return sz;
    }
    if (tag == TAG_BYTES) {
        HeapBytes *b = (HeapBytes *)(uintptr_t)val_payload48(v);
        return sizeof(HeapBytes) + b->len;
    }
    return 0; // immediates (int, nil, true, false, pid, sym)
}
```

### frag_copy — 将 Val 树拷贝到 fragment，返回新的 Val（指针指向 fragment 内部）

```c
static Val frag_copy(MsgFragment *f, Val v) {
    uint16_t tag = val_tag(v);
    if (tag == TAG_PAIR) {
        HeapPair *src = (HeapPair *)(uintptr_t)val_payload48(v);
        Val car = frag_copy(f, src->car);
        Val cdr = frag_copy(f, src->cdr);
        HeapPair *dst = (HeapPair *)(f->data + f->size);
        f->size += sizeof(HeapPair);
        dst->hdr.type = HEAP_PAIR;
        dst->hdr.flags = 0;
        dst->car = car;
        dst->cdr = cdr;
        return box_tag_payload(TAG_PAIR, (uint64_t)(uintptr_t)dst);
    }
    if (tag == TAG_STRING) {
        HeapString *src = (HeapString *)(uintptr_t)val_payload48(v);
        HeapString *dst = (HeapString *)(f->data + f->size);
        f->size += sizeof(HeapString) + src->len + 1;
        dst->hdr.type = HEAP_STRING;
        dst->hdr.flags = 0;
        dst->len = src->len;
        memcpy(dst->data, src->data, src->len);
        dst->data[src->len] = '\0';
        return box_tag_payload(TAG_STRING, (uint64_t)(uintptr_t)dst);
    }
    // closure, bytes 类似...
    return v; // immediates
}
```

### mbox_push (新版)

```c
static void mbox_push(Proc *target, Val msg) {
    int need = frag_calc_size(msg);
    MsgFragment *frag = malloc(sizeof(MsgFragment) + need);
    frag->next = NULL;
    frag->size = 0;
    frag->root = frag_copy(frag, msg);

    pthread_mutex_lock(&target->mbox_lock);
    if (target->mbox_tail) target->mbox_tail->next = frag;
    else                   target->mbox_head = frag;
    target->mbox_tail = frag;
    target->mbox_count++;
    pthread_mutex_unlock(&target->mbox_lock);
}
```

### mbox_pop (新版)

```c
static Val mbox_pop(Proc *p) {
    pthread_mutex_lock(&p->mbox_lock);
    MsgFragment *frag = p->mbox_head;
    p->mbox_head = frag->next;
    if (!p->mbox_head) p->mbox_tail = NULL;
    p->mbox_count--;
    pthread_mutex_unlock(&p->mbox_lock);

    Val v = val_deep_copy(p, frag->root);
    free(frag);
    return v;
}
```

### proc_die 的 DOWN 消息

proc_die 中给 watcher 发 DOWN 消息时，需要在当前进程的堆上构建消息 pair 链，然后用 mbox_push 发送（fragment 拷贝在 mbox_push 内完成）。

当前代码在 watcher 堆上直接构建：
```c
Val msg = val_pair(w, ...);  // 在 w 的堆上分配
mbox_push(w, msg);
```

改为在**当前进程 p** 的堆上构建（p 正在执行，安全），然后 mbox_push 到 watcher：
```c
// 构建在 p 自己的堆上（p 正在被 worker 执行，安全）
Val msg = val_pair(p, ...);
mbox_push(w, msg);  // mbox_push 内部做 fragment 拷贝
```

**关键**：val_pair/p 用的是 p（当前进程），不是 w（目标进程）。mbox_push 负责跨堆拷贝。

## Files to Modify

- `ta.h` — Proc 结构体 mailbox 字段
- `src/vm.c` — mbox_push/mbox_pop 重写, OP_SEND/OP_RECV, proc_new, proc_die, frag_calc_size, frag_copy
- `src/api.c` — vm_free 中释放 mailbox fragments
- `src/val.c` — 检查是否有 mbox 相关引用（可能不需要改）

## Important Constraints

1. **GC 零改动**：gc_collect 和 gc_copy_val 不修改
2. **val_deep_copy 不修改**：它已经支持从任意源指针读取
3. **Fragment 是 malloc'd 内存**：GC 不扫描它，不需要修改 GC root 扫描
4. **锁粒度**：mbox_lock 只保护链表指针操作，不保护 deep_copy
5. **proc_die 中 monitor 消息**：在当前进程 p 的堆上构建，不在 watcher 堆上

## Verification

```bash
# 1. 编译
make clean && make

# 2. 全部测试通过
pass=0; fail=0; for f in test/scripts/*.lisp; do timeout 10 ./tinyactor "$f" >/dev/null 2>&1; if [ $? -eq 0 ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL: $(basename $f)"; fi; done; echo "PASS: $pass FAIL: $fail"

# 3. Concurrent test（关键：验证 fragment 消息传递正确）
timeout 15 ./tinyactor example/scripts/concurrent_test.lisp

# 4. Echo test（验证 send/recv 端到端）
timeout 15 ./tinyactor example/scripts/echo_test.lisp

# 5. GC 不被修改
git diff --stat src/gc.c src/val.c

# 6. Message 内容正确（创建一个新测试验证）
# 需要创建 test/scripts/multithread-msg.lisp:
# - Actor A 发 ('hello "world" 42) 给 Actor B
# - Actor B 收到后验证消息内容正确
# - 输出 PASS
```

## Rules
1. GC 文件不能修改（gc.c, val.c 中的 gc_collect/gc_copy_val）
2. nworkers=1 下零回归（所有测试通过）
3. Fragment 必须 free（recv 后、proc_free 时）
4. Build must pass
5. Output DONE: <file list> when complete