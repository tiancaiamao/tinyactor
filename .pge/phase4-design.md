# Phase 4 设计：多线程调度器 + Heap Fragment + Selective Receive

## 1. 总体架构

```
VM (1 个实例)
├── 全局就绪队列 (runq + mutex + condvar)
├── Worker 线程 1 ─┐
├── Worker 线程 2 ─┤  从 runq 取 actor，执行 MAX_REDUCTIONS 步，放回或等待
├── Worker 线程 N ─┘
├── I/O Poller 线程 (独立，poll WAIT_IO 的 fd，就绪则唤醒)
└── 共享只读数据 (字节码、符号表、C 函数表)
```

**核心不变量（Skynet 规则）：一个 actor 同一时刻只在一个 worker 上执行。**

这条规则让 90% 的代码不需要锁：
- actor 执行（vm_step）：无需锁（独占）
- actor GC（gc_collect）：无需锁（独占）
- actor 栈/堆：无需锁（独占）

**只需要锁的地方：**
- 全局 runq（所有 worker 竞争）
- 每个 actor 的 mailbox（send 方 vs recv 方竞争）

---

## 2. 线程模型

### 2.1 线程数量

```c
int nworkers = vm->nworkers;  // 默认 = sysconf(_SC_NPROCESSORS_ONLN)
```

可配置：`vm_new()` 默认按核数，C 宿主也可以手动设置：
```c
VM *vm = vm_new();
vm->nworkers = 1;  // 退化为单线程（兼容模式）
vm_run(vm);
```

### 2.2 Worker 线程循环

```c
void *worker_loop(void *arg) {
    WorkerCtx *wc = arg;       // 包含 vm + 本线程的 current_proc
    VM *vm = wc->vm;

    for (;;) {
        int pid = runq_dequeue(vm);   // 阻塞直到有 actor 或 shutdown
        if (pid < 0) break;           // shutdown 信号

        Proc *p = vm->procs[pid];
        if (!p || p->state != PROC_RUNNING) continue;

        wc->current_proc = p;         // 替代全局 vm->current_proc

        // 执行 MAX_REDUCCTIONS 步
        for (int r = 0; r < MAX_REDUCTIONS; r++) {
            if (vm_step(vm, p, wc) != 0) break;  // 停止信号
        }

        // 根据状态决定后续
        switch (p->state) {
        case PROC_RUNNING:
            runq_enqueue(vm, p->pid);  // 时间片用完，放回队列
            break;
        case PROC_WAIT_RECV:
        case PROC_WAIT_IO:
            // 不放回队列，等待唤醒
            break;
        case PROC_DEAD:
            // proc_die 已处理清理
            break;
        }
    }
    return NULL;
}
```

### 2.3 vm_run 改造

```c
void vm_run(VM *vm) {
    // 1. 初始化全局 runq（已经有 main 进程入队）
    
    // 2. 启动 I/O poller 线程
    pthread_create(&vm->io_thread, NULL, io_poller_loop, vm);
    
    // 3. 启动 N 个 worker 线程
    vm->workers = malloc(vm->nworkers * sizeof(pthread_t));
    vm->wctxs   = malloc(vm->nworkers * sizeof(WorkerCtx));
    for (int i = 0; i < vm->nworkers; i++) {
        vm->wctxs[i].vm = vm;
        vm->wctxs[i].thread_id = i;
        vm->wctxs[i].current_proc = NULL;
        pthread_create(&vm->workers[i], NULL, worker_loop, &vm->wctxs[i]);
    }
    
    // 4. 等待所有 worker 结束
    for (int i = 0; i < vm->nworkers; i++)
        pthread_join(vm->workers[i], NULL);
    
    // 5. 停止 I/O poller
    vm->stop = 1;
    pthread_join(vm->io_thread, NULL);
}
```

### 2.4 退出条件（最微妙的问题）

什么时候所有 worker 停止？

```
活跃 actor 计数 = RUNNING 队列中 + WAIT_RECV 邮箱可能有消息 + WAIT_IO fd 可能就绪
```

用原子计数器：

```c
// VM 中新增
atomic_int active_procs;   // 所有非 DEAD 且非"永久阻塞"的进程数

// spawn 时：active_procs++
// proc 死亡时：active_procs--
// 当 active_procs == 0：broadcast condvar，所有 worker 退出
```

注意：WAIT_RECV 和 WAIT_IO 的进程仍然算 active（它们可能被唤醒）。
只有 DEAD 的进程不算 active。
一个 forever-running 的 accept-loop actor 会让 active_procs 永远 > 0，
这时 VM 永远不退出——这是正确的行为（服务端程序）。

对于 finite test（如 concurrent_test.lisp）：所有 actor 正常退出后，
active_procs 降到 0，VM 自然终止。

---

## 3. 全局就绪队列（runq）

### 3.1 当前实现

```c
int *runq;           // pid 的环形 buffer
int rq_head, rq_tail, rq_count, rq_cap;
// 全部无锁，单线程操作
```

### 3.2 改造后

```c
// VM 中新增
pthread_mutex_t rq_lock;
pthread_cond_t  rq_cond;     // worker 等待新 actor
int            *rq_buf;      // pid 环形 buffer（大小预分配，避免运行时 realloc）
int             rq_cap;      // 容量（固定 = max_procs）
int             rq_head;     // 消费端
int             rq_tail;     // 生产端
atomic_int      rq_count;    // 当前元素数
```

### 3.3 操作

```c
// 入队（被 spawn、send唤醒、poller唤醒调用）
void runq_enqueue(VM *vm, int pid) {
    pthread_mutex_lock(&vm->rq_lock);
    // 环形 buffer 已满 → 扩容（realloc，罕见）
    vm->rq_buf[vm->rq_tail % vm->rq_cap] = pid;
    vm->rq_tail++;
    atomic_fetch_add(&vm->rq_count, 1);
    pthread_cond_signal(&vm->rq_cond);     // 唤醒一个等待的 worker
    pthread_mutex_unlock(&vm->rq_lock);
}

// 出队（worker 调用，阻塞）
int runq_dequeue(VM *vm) {
    pthread_mutex_lock(&vm->rq_lock);
    while (atomic_load(&vm->rq_count) == 0 && !vm->stop) {
        // 检查是否所有进程都已死亡
        if (atomic_load(&vm->active_procs) == 0) {
            vm->stop = 1;
            pthread_cond_broadcast(&vm->rq_cond);
            break;
        }
        pthread_cond_wait(&vm->rq_cond, &vm->rq_lock);
    }
    if (vm->stop && atomic_load(&vm->rq_count) == 0) {
        pthread_mutex_unlock(&vm->rq_lock);
        return -1;   // shutdown
    }
    int pid = vm->rq_buf[vm->rq_head % vm->rq_cap];
    vm->rq_head++;
    atomic_fetch_sub(&vm->rq_count, 1);
    pthread_mutex_unlock(&vm->rq_lock);
    return pid;
}
```

**锁持有时间**：纳秒级（一次数组读写 + 原子操作）。
N 个 worker 碰撞概率：在 4-8 核上可忽略。

### 3.4 runq 扩容竞争

runq 的 realloc 操作需要在 rq_lock 保护下完成（已经在 enqueue 内部）。
但由于 dequeue 也在等 rq_lock，所以 realloc 期间 dequeue 被阻塞。
这是安全的，只是偶尔有短暂延迟。

更好方案：预分配大容量（65536 slots），永不扩容。每个 slot 4 字节，
总共 256KB，完全可接受。

---

## 4. Heap Fragment（消息传递）

### 4.1 问题回顾

当前 send 在目标进程的堆上深拷贝消息。多线程下，目标进程的堆可能
正在被另一个 worker 的 GC 翻转——竞态。

### 4.2 核心思路

**send 不在目标堆上分配。在 malloc'd fragment 上分配。**

fragment 是一块独立的堆内存，不属于任何进程的 semispace。
它通过 mailbox 链表挂到目标进程。recv 时从 fragment 反序列化到自己的堆。

```
线程 1 执行 Actor A             线程 2 执行 Actor B
──────────────────             ──────────────────
A 调用 send(B, msg)
  ↓
  1. malloc MsgFragment         B 正常运行
  2. 在 fragment 内拷贝 msg      B 可能触发自己的 GC
     （不碰 B 的堆）              （完全安全，B 只碰自己的堆）
  3. 锁 B->mbox_lock
  4. fragment 挂入 B 的 mailbox
  5. 解锁 B->mbox_lock
  6. 如果 B 在 WAIT_RECV → 唤醒
                                ...
                                B 调用 recv
                                  ↓
                                  1. 锁自己的 mbox_lock（自己是执行者，无竞争）
                                  2. 从 mailbox 取 fragment
                                  3. 解锁
                                  4. val_deep_copy(B, fragment->val)
                                     → 在 B 自己的堆上分配（安全）
                                  5. free(fragment)
                                  6. push Val 到 B 的栈
```

### 4.3 数据结构

```c
// 消息碎片：一块 malloc'd 内存，内含紧凑排列的堆对象
typedef struct MsgFragment {
    struct MsgFragment *next;     // 链表
    int    size;                  // data 区已用字节数（bump pointer）
    Val    root;                  // 根 Val（指向 data 内的对象）
    uint8_t data[];               // 柔性数组：堆对象布局
} MsgFragment;

// Proc 的 mailbox 改为：
typedef struct Proc {
    ...
    // 替代原来的 Val *mbox 环形 buffer
    MsgFragment *mbox_head;       // 消息链表头
    MsgFragment *mbox_tail;       // 消息链表尾
    int          mbox_count;      // 消息数
    pthread_mutex_t mbox_lock;    // 保护 push/pop 操作
    ...
};
```

### 4.4 Fragment 分配器（send 侧）

```c
// 在 fragment 内部分配（bump allocator）
static void *frag_alloc(MsgFragment *f, int size) {
    void *ptr = f->data + f->size;
    f->size += size;
    return ptr;
}

// 计算 Val 树需要的字节数（不含 Val 本身，含堆对象）
static int frag_calc_size(Val v) {
    uint16_t tag = val_tag(v);
    switch (tag) {
    case TAG_INT: case TAG_NIL: case TAG_TRUE:
    case TAG_FALSE: case TAG_PID: case TAG_SYM:
        return 0;  // immediate，不需要堆空间
    }
    if (tag == TAG_PAIR) {
        HeapPair *src = (HeapPair *)(uintptr_t)val_payload48(v);
        return sizeof(HeapPair)
             + frag_calc_size(src->car)
             + frag_calc_size(src->cdr);
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
    return 0;
}

// 将 Val 树拷贝到 fragment（保留堆对象布局）
// 返回的 Val 的指针指向 fragment->data 内部
static Val frag_copy(MsgFragment *f, Val v) {
    uint16_t tag = val_tag(v);
    switch (tag) {
    case TAG_INT: case TAG_NIL: case TAG_TRUE:
    case TAG_FALSE: case TAG_PID: case TAG_SYM:
        return v;  // immediate 原样返回
    }
    if (tag == TAG_PAIR) {
        HeapPair *src = (HeapPair *)(uintptr_t)val_payload48(v);
        Val car = frag_copy(f, src->car);  // 递归先拷子树
        Val cdr = frag_copy(f, src->cdr);
        HeapPair *dst = frag_alloc(f, sizeof(HeapPair));
        dst->hdr.type = HEAP_PAIR;
        dst->hdr.flags = 0;
        dst->car = car;
        dst->cdr = cdr;
        return box_tag_payload(TAG_PAIR, (uint64_t)(uintptr_t)dst);
    }
    if (tag == TAG_STRING) {
        HeapString *src = (HeapString *)(uintptr_t)val_payload48(v);
        HeapString *dst = frag_alloc(f, sizeof(HeapString) + src->len + 1);
        dst->hdr.type = HEAP_STRING;
        dst->hdr.flags = 0;
        dst->len = src->len;
        memcpy(dst->data, src->data, src->len);
        dst->data[src->len] = '\0';
        return box_tag_payload(TAG_STRING, (uint64_t)(uintptr_t)dst);
    }
    // closure 类似...
    return v;
}
```

### 4.5 send 改造

```c
case OP_SEND: {
    Val pid_v = proc_pop(p);
    Val msg   = proc_pop(p);
    Proc *t   = vm->procs[val_get_pid(pid_v)];
    if (!t || t->state == PROC_DEAD) break;

    // 1. 在 fragment 上拷贝消息（malloc，线程安全）
    int need = frag_calc_size(msg);
    MsgFragment *frag = malloc(sizeof(MsgFragment) + need);
    frag->next = NULL;
    frag->size = 0;
    frag->root = frag_copy(frag, msg);  // root 指向 fragment 内部

    // 2. 挂入目标 mailbox
    pthread_mutex_lock(&t->mbox_lock);
    if (t->mbox_tail) t->mbox_tail->next = frag;
    else              t->mbox_head = frag;
    t->mbox_tail = frag;
    t->mbox_count++;
    int need_wake = (t->state == PROC_WAIT_RECV);
    pthread_mutex_unlock(&t->mbox_lock);

    // 3. 如果目标在等消息，唤醒它
    if (need_wake) {
        t->state = PROC_RUNNING;
        runq_enqueue(vm, t->pid);
    }
    break;
}
```

**关键：frag_copy 和 malloc 完全不碰目标进程的堆，无 GC 竞态。**

### 4.6 recv 改造

```c
case OP_RECV: {
    if (p->mbox_count == 0) {
        p->pc--;  // 回退 PC，下次重新执行 OP_RECV
        p->state = PROC_WAIT_RECV;
        return -1;
    }

    // 从 mailbox 取一个 fragment
    pthread_mutex_lock(&p->mbox_lock);
    MsgFragment *frag = p->mbox_head;
    p->mbox_head = frag->next;
    if (!p->mbox_head) p->mbox_tail = NULL;
    p->mbox_count--;
    pthread_mutex_unlock(&p->mbox_lock);

    // 从 fragment 反序列化到自己的堆（可能触发自己的 GC，安全）
    Val v = val_deep_copy(p, frag->root);
    free(frag);

    proc_push(p, v);
    break;
}
```

**关键：val_deep_copy 在自己的线程上、自己的堆上分配。**
**源指针指向 fragment（malloc'd），val_deep_copy 不关心源在哪，只跟随指针读数据。**

### 4.7 GC 零改动证明

为什么 GC 不需要改？

```
1. GC 只扫描自己的 gc_roots[] 和 stack[]，这些都在自己线程控制下
2. mailbox 里的 fragment 是 malloc'd 内存，不是 semispace，GC 不扫它
3. recv 从 fragment 取出数据后立即 free(frag)，fragment 不持久化
4. val_deep_copy 期间如果触发 GC：
   - gc_root_push 保护部分构建的值
   - 源指针指向 fragment（不会被 GC 回收）
   - 目标分配在自己的 semispace 上（安全）
5. fragment 中的 Val 永远不会出现在进程的 stack 或 gc_roots 上
   （recv 总是先 deep_copy 到堆，再 push 到栈）
```

**结论：gc_collect() 函数一行都不用改。**

### 4.8 proc_die 的 monitor 消息

proc_die 也用 fragment 发 DOWN 消息：

```c
static void proc_die(VM *vm, Proc *p, Val reason) {
    p->state = PROC_DEAD;
    // ... close wait_fd if needed ...
    
    for (int i = 0; i < p->watcher_count; i++) {
        Proc *w = vm->procs[p->watchers[i]];
        if (!w || w->state == PROC_DEAD) continue;
        
        // 用 fragment 发送 DOWN 消息
        // 构建 ('DOWN ref pid reason) 到 fragment
        int need = ...;  // 计算 size
        MsgFragment *frag = malloc(sizeof(MsgFragment) + need);
        frag->size = 0;
        // 构建 pair 链到 fragment
        // ...（省略构建过程）
        
        pthread_mutex_lock(&w->mbox_lock);
        // ... push to w's mailbox ...
        pthread_mutex_unlock(&w->mbox_lock);
        
        if (w->state == PROC_WAIT_RECV) {
            w->state = PROC_RUNNING;
            runq_enqueue(vm, w->pid);
        }
    }
    atomic_fetch_sub(&vm->active_procs, 1);
}
```

---

## 5. I/O Poller 线程

### 5.1 架构

```c
void *io_poller_loop(void *arg) {
    VM *vm = arg;
    while (!vm->stop) {
        struct pollfd pfds[1024];
        int pids[1024];
        int nfds = 0;

        // 扫描 WAIT_IO 进程（读状态是安全的，见 5.2）
        for (int i = 0; i < vm->procs_cap && nfds < 1024; i++) {
            Proc *p = vm->procs[i];
            if (p && p->state == PROC_WAIT_IO) {
                pfds[nfds].fd      = p->wait_fd;
                pfds[nfds].events  = p->wait_events;
                pfds[nfds].revents = 0;
                pids[nfds]         = i;
                nfds++;
            }
        }

        if (nfds == 0) {
            usleep(1000);  // 1ms 空转
            continue;
        }

        poll(pfds, nfds, 100);

        for (int i = 0; i < nfds; i++) {
            if (pfds[i].revents & (POLLIN | POLLOUT | POLLERR | POLLHUP)) {
                Proc *p = vm->procs[pids[i]];
                // 安全性见 5.2
                if (p && p->state == PROC_WAIT_IO) {
                    p->state = PROC_RUNNING;
                    runq_enqueue(vm, p->pid);
                }
            }
        }
    }
    return NULL;
}
```

### 5.2 为什么 poller 扫描 procs[] 不需要锁

状态转换分析：

```
→ PROC_WAIT_IO:   由 worker 设置（此时 actor 被该 worker 独占，不在 runq 中）
                  设置后 actor 不在 runq，没有 worker 会碰它
→ PROC_RUNNING:   由 poller 设置（唯一从 WAIT_IO 唤醒的路径）
                  设置后入 runq，等待 worker 取走

死锁可能？无。
  - worker 设 WAIT_IO 后不再碰该 actor → poller 安全读
  - poller 设 RUNNING 后不再碰该 actor → worker 安全取

竞态可能？无。
  - procs[i]->state 是简单的 int 赋值（原子性在 x86/ARM 上保证）
  - state 只有"所有者"才能改：RUNNING→WAIT_IO 的所有者是 worker，
    WAIT_IO→RUNNING 的所有者是 poller
```

### 5.3 procs[] 数组的线程安全

`procs[]` 本身可能被 spawn 扩容（realloc）。解决方案：

**预分配固定大小**，永不 realloc：

```c
#define MAX_PROCS 65536  // 16-bit pid 空间

// vm_new:
vm->procs = calloc(MAX_PROCS, sizeof(Proc*));
vm->procs_cap = MAX_PROCS;
```

- 内存开销：65536 × 8 字节 = 512KB（可接受）
- spawn 只需要写入 `vm->procs[pid] = p`（原子指针写）
- poller 读 `vm->procs[i]`（原子指针读）
- 无需锁

### 5.4 would-block → PROC_WAIT_IO 的路径

当前 OP_CCALL 拦截 would-block：

```c
// OP_CCALL handler（简化）
Val result = cfuncs[op-func_id].fn(vm, args, nargs);
if (val_is_symbol(result) && result == sym_would_block) {
    // 当前 worker 设置
    p->state = PROC_WAIT_IO;
    p->wait_fd = vm->last_wait_fd;
    p->wait_events = vm->last_wait_events;
    p->pc -= instruction_size;  // 回退 PC，下次重新执行
    return -1;  // 退出 reduction 循环
}
```

这段在 worker 线程执行，actor 被该 worker 独占。设完状态后
return -1 退出循环，worker 不再把 actor 放入 runq。
poller 下次扫描时会发现这个 WAIT_IO actor。

**零改动，天然兼容。**

---

## 6. spawn 线程安全

### 6.1 pid 分配

```c
// 从全局原子计数器分配
int new_pid = atomic_fetch_add(&vm->next_pid, 1);
```

### 6.2 proc 创建

proc_new 在当前 worker 线程中执行，创建的 Proc 是全新的，
没有其他线程能看到它，直到它被写入 vm->procs[]。

```c
Proc *p = proc_new(vm);  // 全程在当前线程，无竞争
// ... 初始化 code, stack, heap ...
vm->procs[p->pid] = p;   // 原子发布（其他线程可见）
atomic_fetch_add(&vm->active_procs, 1);
runq_enqueue(vm, p->pid);  // 入队，等待 worker 取走
```

**proc_new 内部的 procs[] realloc 问题：**
预分配 MAX_PROCS 后不存在 realloc，直接写入即可。

### 6.3 OP_SPAWN 改造

```c
case OP_SPAWN: {
    Val clos_val = proc_pop(p);
    Proc *np = proc_new(vm);
    // ... 设置 np 的 pc, stack, closure ...
    vm->procs[np->pid] = np;
    atomic_fetch_add(&vm->active_procs, 1);
    runq_enqueue(vm, np->pid);
    proc_push(p, val_pid(np->pid));
    break;
}
```

改动极小。

---

## 7. current_proc → per-thread

### 7.1 问题

当前 `vm->current_proc` 是全局变量。多线程下每个 worker 有自己的
当前进程。

### 7.2 方案：WorkerCtx

```c
typedef struct {
    VM   *vm;
    Proc *current_proc;    // 每个线程自己的
    int   thread_id;
} WorkerCtx;
```

vm_step 接受 WorkerCtx 而非 VM 作为第一个参数：

```c
int vm_step(WorkerCtx *wc, Proc *p) {
    VM *vm = wc->vm;
    // ... 所有需要 current_proc 的地方用 wc->current_proc
}
```

**需要 current_proc 的地方只有：**
- C 函数调用（OP_CCALL）中 net_read 需要 `p = vm->current_proc`
  → 改为从 WorkerCtx 获取，或通过函数参数传入

实际上，net_read 用 current_proc 是为了 GC 分配字符串：
```c
Val result = val_string(p, buf, n);  // 需要在 p 的堆上分配
```

改成从参数传入即可。C 函数签名可以加一个隐式的 Proc* 参数，
或者通过 WorkerCtx 传递。

### 7.3 影响范围

```c
// OP_CCALL handler 中：
// 旧：Val result = fn(vm, args, nargs);
// 新：Val result = fn(wc, args, nargs);  // 第一个参数改为 WorkerCtx*

// 或更小改动：保持 VM* 参数，但 vm->current_proc 改为 thread-local
static __thread Proc *tls_current_proc;
// vm_step 入口：tls_current_proc = p;
// C 函数中：Proc *p = tls_current_proc;
```

**推荐 thread-local 方案**：改动最小，C 函数签名不变。
但需要 C11 或 GCC __thread 扩展。当前用 C99，可以用 pthread_key_t 替代。

---

## 8. Selective Receive

### 8.1 当前 recv 的问题

```lisp
(recv)  ;; 取第一条消息，不管是什么
```

无法跳过不想要的消息。例如等特定 ref 的回复时，
邮箱里可能堆积了其他消息。

### 8.2 设计：pattern-based recv

```lisp
;; 新语法：recv 带模式匹配，扫描整个 mailbox
(receive
  (('ping from)  (send from 'pong))
  ('stop         (exit 0))
  (msg           (print msg)))  ;; default（catch-all）

;; 旧语法保留兼容
(recv)  ;; 仍可用，取第一条
```

### 8.3 实现

**编译器改动**（compile.c）：
- `receive` 特殊形式编译为 OP_RECV_SCAN + N 个 match 子句
- 或复用现有 match 机制：先编译一个 lambda，对 mailbox 中每条消息执行 match

**VM 改动**（vm.c）：
- 新增 OP_RECV_SCAN：扫描 mailbox，对每条消息执行 pattern matching
- 第一条匹配的取出，不匹配的保留在 mailbox

**mailbox 数据结构适配：**
- fragment 链表天然支持扫描（遍历链表，不 pop）
- 匹配的消息从链表中摘除，不匹配的保留

```c
case OP_RECV_SCAN: {
    int pattern_pc = ...;  // pattern matching 代码的 PC
    
    pthread_mutex_lock(&p->mbox_lock);
    MsgFragment *prev = NULL, *cur = p->mbox_head;
    while (cur) {
        Val v = val_deep_copy_temp(p, cur->root);  // 临时拷贝到栈用于匹配
        if (match_pattern(p, v, pattern_pc)) {
            // 匹配！从链表摘除
            if (prev) prev->next = cur->next;
            else      p->mbox_head = cur->next;
            if (cur == p->mbox_tail) p->mbox_tail = prev;
            p->mbox_count--;
            
            // 正式拷贝到堆
            Val result = val_deep_copy(p, cur->root);
            free(cur);
            proc_push(p, result);
            pthread_mutex_unlock(&p->mbox_lock);
            goto matched;
        }
        prev = cur;
        cur = cur->next;
    }
    pthread_mutex_unlock(&p->mbox_lock);
    
    // 没有匹配，等待
    p->state = PROC_WAIT_RECV;
    p->pc--;  // 重试
    return -1;
    
matched:
    break;
}
```

### 8.4 优先级

selective receive 在多线程改造之后做，作为独立 task。
它不与多线程逻辑冲突，但增加了 mailbox 操作的复杂度。

---

## 9. 数据结构变更汇总

### ta.h

```c
// 新增：消息碎片
typedef struct MsgFragment {
    struct MsgFragment *next;
    int   size;
    Val   root;
    uint8_t data[];
} MsgFragment;

// Proc 改动
typedef struct Proc {
    ...
    // mailbox：从 Val 环形 buffer 改为 fragment 链表
    // 旧：Val *mbox; int mbox_head, mbox_tail, mbox_count, mbox_cap;
    MsgFragment *mbox_head;
    MsgFragment *mbox_tail;
    int          mbox_count;
    pthread_mutex_t mbox_lock;
    ...
} Proc;

// VM 改动
struct VM {
    ...
    // runq：加锁
    pthread_mutex_t rq_lock;
    pthread_cond_t  rq_cond;
    
    // pid：原子
    atomic_int next_pid;  // 替代 int next_pid
    
    // active 计数：原子
    atomic_int active_procs;
    
    // 线程管理
    pthread_t io_thread;
    pthread_t *workers;
    int nworkers;
    int stop;
    
    // 删除 current_proc（改为 thread-local 或 WorkerCtx）
    
    // procs[]：固定大小，不 realloc
    // procs_cap = MAX_PROCS (65536)
    ...
};
```

### 新文件

无新文件。所有改动在现有文件内（ta.h, vm.c, gc.c, val.c, compile.c）。

---

## 10. 不需要改的部分

| 组件 | 理由 |
|------|------|
| gc_collect() | per-process semispace，worker 独占执行 |
| gc_copy_val() | 只操作当前进程的两个 semispace |
| val_deep_copy() | 源指针指向哪都行，只跟随指针读数据 |
| val_pair/string/closure() | 在当前进程堆上分配，用 gc_root_push 保护 |
| compile.c / reader.c | 编译阶段在 vm_run 之前完成，无并发 |
| net.c / http.c / module.c | C 函数在 worker 线程执行，actor 独占 |
| 字节码 / 符号表 / 函数表 | 只读，加载后不再修改 |
| NaN-boxing / 值表示 | 纯数据格式，线程无关 |

---

## 11. 改动量估算

| 改动点 | 文件 | 行数 | 风险 |
|--------|------|------|------|
| runq + mutex + condvar | vm.c | ~60 | 低 |
| Heap fragment + send/recv | vm.c + ta.h + val.c | ~120 | 中 |
| Worker 线程循环 + vm_run | vm.c | ~80 | 低 |
| I/O poller 独立线程 | vm.c | ~50 | 低 |
| spawn / proc_die / monitor | vm.c | ~40 | 低 |
| current_proc → thread-local | vm.c + ta.h | ~20 | 低 |
| procs[] 固定大小 | vm.c | ~10 | 低 |
| 编译选项（-lpthread） | Makefile | ~3 | 低 |
| **合计** | | **~380** | |

加上测试：
- 多线程 echo server 测试
- 多线程 concurrent_test（多 client 并发，验证 worker 真正并行）
- 死锁检测测试
- selective receive 测试

测试预估 ~200 行，总计 ~580 行。

---

## 12. 实施顺序

```
Task 1: 基础线程设施
  - ta.h: MsgFragment, pthread_mutex_t, atomic_int 等
  - vm.c: runq 加锁、vm_run 启动线程
  - 先用 nworkers=1 验证不回归（退化为单线程）
  - 验收：所有 46 个测试通过

Task 2: Heap Fragment 消息传递
  - ta.h: mailbox 改为 fragment 链表
  - vm.c: send/recv/proc_die 改用 fragment
  - val.c: frag_calc_size, frag_copy
  - 验收：单线程下所有测试通过（fragment 不影响逻辑）

Task 3: 多 worker 线程
  - vm.c: worker_loop, current_proc → thread-local
  - nworkers > 1 启用多线程
  - 验收：concurrent_test 通过，多线程 echo server 通过

Task 4: I/O poller 独立线程
  - vm.c: io_poller_loop 从 vm_run 分离
  - 验收：HTTP server 多 worker 下正常

Task 5: Selective Receive
  - compile.c: receive 特殊形式
  - vm.c: OP_RECV_SCAN
  - 验收：selective receive 测试通过

Task 6: 技术债清理
  - net.c 独立编译（不写死在 VM 中）
  - bytes 类型修复或移除
  - 文档更新
```

---

## 13. 风险与缓解

| 风险 | 概率 | 缓解 |
|------|------|------|
| 竞态条件导致随机 crash | 高 | 先用 nworkers=1 跑通全部回归，再开多线程 |
| 死锁（worker 等 condvar，没有人唤醒） | 中 | active_procs == 0 时 broadcast 所有 worker |
| mailbox lock 竞争影响性能 | 低 | 锁持有时间极短（链表指针操作） |
| fragment 内存泄漏（recv 前进程死亡） | 中 | proc_die 时释放 mailbox 所有 fragment |
| I/O poller 扫描延迟（1ms 空转） | 低 | 可改用 epoll/kqueue 优化（未来） |