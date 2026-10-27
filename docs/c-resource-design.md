# C 资源：不透明值 + 归属移交 + GC 兜底

状态：设计稿 v3（v1 经 R1 评审修 5 处严重缺陷；v2 经 R2 复审，S1–S5 修正全部成立、
无新增严重缺陷，按 R2 前置条件修订。见 §11）。对应 issue #401。

## 1. 问题

`lib/buffer.c` / `process.c` / `tls.c` 现在是同一套「模块级槽表 + 裸 int 句柄 + 一把
全局锁」：

```c
typedef struct { uint8_t *data; int len, cap, gen; } BufSlot;
static BufSlot bufs[1 << 8];          /* 槽表：全模块共用 */
static int next_slot;                 /* 下一个空槽 */
static int free_head; static int free_next[1 << 8];   /* 空闲链表 */
static pthread_mutex_t g_buf_lock;    /* 一把锁护住上面全部 */
```

句柄编码 `slot | gen << 8`，`gen` 用来防 ABA；256 槽用尽返回 `-1`。这套东西的唯一
职责，是把 int 句柄映射回指针。

它违反两条：

1. `AGENTS.md`「C 模块不得有全局状态」—— 模块级表 + 全局锁把「这份状态归谁」变成
   「谁来加锁」；
2. 锁的实际保护对象只是那张表，不是资源本身。`buffer_raw_to_string` 在锁内调
   `val_string`（分配 → GC）只是顺着写下来的，锁的存在让 GC 与锁交叉。

根因是错配：**资源只有一份，句柄却是 int（可任意复制、可跨 actor 传递）**。补丁
（表 + 锁）是对这个错配的补偿。

## 2. 目标 / 非目标

**目标**

- C 模块零全局变量、零锁：删掉三张槽表、三把锁、free list、gen/ABA 机制；
- 资源可变（buffer 原地 append，性能不变），但归属确定：任意时刻只有一个 actor 有权
  使用它；
- 释放有两条腿：显式 `close()`（生命周期与 actor 不一致时用）+ 兜底（忘记 close 不
  泄漏，见 §6 四条路径）；
- 语言层只有一个新的不透明类型 `Resource`，`lib/*.ta` 的 API 形状不变（`Buf(Int)` →
  `Buf(Resource)`，其余照旧）。

**非目标**

- 不做「共享资源」：资源跨 actor 只能移交，不能同时使用。多持有者模型（引用计数）本
  设计不采用；
- 不做语言层/运行时层的安全承诺：谁该用、能不能并发用，由写代码的人保证。运行时只保
  证「不泄漏、不 double-free、已释放后操作报错而不是崩」；
- 不做 `Resource('kind)` 参数化类型；
- 不改 OpenSSL / libc：`lib/tls.c`、`process.c` 只是把我们自己的胶水层从槽表换成资源
  值。

## 3. 值形态

```c
/* ta.h */
#define HEAP_RESOURCE 5          /* 新堆对象类型 */
#define TAG_RESOURCE  0xFFFC     /* NaN-boxing tag：FFF1–FFFB 已用，TAG_LAST 同步更新 */

/* 堆外 malloc 的共享资源身份记录：跨副本共享，是 released 的唯一所在地 */
typedef struct {
    void *ptr;           /* 模块私有的可变资源本体（buffer 字节 / fd / SSL* …） */
    void (*dtor)(void *);
    uint8_t released;    /* 已释放：dtor 已跑，所有副本都失效 */
} ResourcePayload;       /* 24 字节/资源，永不 free（§7 裁决 ③） */

/* actor 堆里的 header：每个持有者一份，会被 GC 搬运 */
typedef struct {
    HeapHeader h;
    int32_t owner_pid;          /* 谁负责释放；只有当前属主可写 */
    ResourcePayload *res;       /* 指向共享 payload */
} HeapResource;
```

**每个持有者一份 header，payload 共享。** header 是 actor 堆里的普通对象（会被 GC
搬运），payload 是堆外 malloc 的可变内存/句柄 + 身份记录。

```
actor A 堆:  HeapResource(owner=B, res=P) ──┐
actor B 堆:  HeapResource(owner=B, res=P) ──┴──► P { ptr=X, dtor, released } ──► X（可变本体）
```

为什么是「每持有者一份 header + 共享 payload」而不是「共享一个 header + 引用计数」：

- 发送方的旧副本留在自己堆里（半空间复制 GC 无法原地改写、也无法删除），它是
  `owner_pid != 自己` 的 header —— GC 扫到就跳过，不碰 payload；
- 于是**任何共享的可变字段都不在 actor 堆里**，`released` 是唯一的跨副本共享状态，
  它在堆外 payload 里，写者永远是当前属主（同线程）。BEAM 的引用计数（`refs`）在移
  交语义下消失，atomic 也随之消失（§5）。

`HeapResource` 是普通堆对象，现有机制原样适用：GC 搬运、`obj_size` 一行、
`val_calc_heap_size` 一行。payload 不参与搬运。

**`released` 为什么必须在 payload 侧**（v1 的教训）：`closed` 若放在 header 里，
「同一个资源出现在两份 header 副本里」时两份各自 `closed=0`，第二次释放会 double-
free。合法单归属流内部就会产生多副本（`cons(r, r)`、`b_recv_peek` 不弹出 fragment），
所以释放标志必须共享。代价：`ResourcePayload` 结构体本身不回收（真实资源在
`released` 时释放，这 24 字节身份记录活到 VM 退出）——换来的是「副本访问已释放的
资源」报错而不是读到已释放内存。见 §11 S4。

**S2 与 S4 是耦合修复**（R2）：禁 self-send 挡住的是最常见的「双份 owner==self」
路径，但存在不经过 self-send 的等价路径（B 回送 + A 的 selective-receive peek，见
§4 peek 禁令）——该状态的统一兜底是 `released`，不要把 S2 当作独立完整的封死，
更不能单独回归 S4。

## 4. 归属与移交

**规则：资源的归属在投递成功时移交给接收方。** 接收方 = 消息所在的邮箱所属的
actor，或 spawn 出来的新 actor。

**移交时由属主线程同时改写三处**（v1 只改了 fragment，是严重缺陷，见 §11 S1）：

1. **自己堆里的旧 header**：`owner_pid = 接收方`。属主线程写自己的堆，不变量 H 不破；
2. **fragment 里每个副本**：`cons(r, r)` 有几个副本改几个；
3. **接收方堆里的副本**（spawn 路径）：`owner_pid = 子 pid`。

投递路径（`mbox_deliver` 是收口，4 个调用点全部核实：`src/vm.c:175` LB_SEND、
`src/scheduler.c:542` DOWN 广播、`src/builtin.c:196` b_monitor 即时 DOWN、
`lib/timer.c` fire）：

| 路径 | 代码位置 | 归属改写时机 |
|---|---|---|
| `send(pid, msg)` | `mbox_deliver`（`src/scheduler.c:223-243`） | `mbox_lock` 内**入队成功后**改写：发送方自己堆 header + fragment 每个副本。入队失败（`PROC_DEAD` → `free(frag)`，`scheduler.c:232-235`）**不动**发送方堆 header，归属留在发送方 |
| spawn | `LB_SPAWN`（`src/vm.c:145-167`） | `val_deep_copy(np, clos)`（163）之后、`runq_enqueue`（165）之前改写父堆 header + 子堆副本。[163, 165] 窗口内两个堆都不移动（reserve 的是子堆、frame_init 只做栈操作），改写地址稳定；子进程尚不可运行，无并发 |
| `recv` | `mbox_pop`（`src/scheduler.c:254-268`） | 不改。归属已在投递时写好，`val_deep_copy` 原样拷 header |
| timer | `lib/timer.c:126-150`（register）、fire 路径 | **不支持**（见下）。fire 时 `msg = t->frag->root` 指向 malloc 快照而非任何堆，改写是 no-op——依赖「register 已拒绝资源」使该分支不可达 |

**实现前提**（R2 S1 节）：`mbox_deliver` 手里只有 `(vm, target, msg)`，要改写「发送
方自己堆 header」需要**第三次遍历 msg 树**（`frag_calc_size`、`frag_copy` 之外再加
一遍）。可行，§8 触点清单已列。改写与入队在同一次 `mbox_lock` 持有内、同一线程完
成，接收方 `mbox_pop` / peek 要拿同一把锁，happens-before 由 mutex 保证；发送方死亡
只有 `PROC_DIE_ISOLATED`（`src/vm.c:97-105`，6 处调用全在 run_proc 内）和入口帧返回
（`src/vm.c:1023-1025`）两条路，都跑在本 proc 自己的线程上，不可能与半途的
mbox_deliver 并发；接收方死亡的 `PROC_DEAD` 置位（`scheduler.c:429`）早于 drain
（546-561），两种交错都封死。建议实现时把改写挪到唤醒（242）之前，纯观感。

**归属检查收口在 `frag_copy` / `val_deep_copy` 的资源分支**：遇到 `HEAP_RESOURCE`
时校验 `owner_pid == tls_current_proc->pid`，否则报错「resource owned by actor N」。
这一处校验同时实现三件事：

- 「第二次 send 在源头报错」（fan-out 循环发送同一资源）；
- 「移交后父方/发送方继续用」的下一次 send 会被拦住（不是静默）；
- 拦掉两个必须禁止的路径（见下）。

校验必须 **NULL-safe**（R2 N3）：`tls_current_proc` 在 timer fire 的 poller 线程上是
NULL（`src/scheduler.c:645` 的 io_poller_thread 从不设置它；全仓库唯一赋值点
`src/vm.c:354`），单线程模式 fire 时是陈旧值。资源分支虽因 register 已拒而不可达，
校验仍要写成 `tls_current_proc == NULL → fatal(带清晰信息)`，让未来的回归以报错而
不是段错误的方式暴露。

**禁止：资源 send 给自己。** `target == self` 时移交是空操作，但 GC 不扫邮箱
（根集只有栈，`src/gc.c:194-203`）——发送方堆里的副本成垃圾后被 GC 释放，而自己
邮箱里的 fragment 还持有活引用 → UAF + double-free（§11 S2）。`frag_copy` 收口处
`target == self && 遇到 HEAP_RESOURCE` 直接报错。自己已持有资源，发给自己语义上是
no-op，拒绝没有损失。**注意**：此禁令不覆盖 peek 等价路径（见下），文档不要声称
「完整封死双 owner==self」。

**禁止：资源出现在 timer 消息里。** timer 是第四个投递路径，且有 send 没有的两个毒
性：① 延迟（注册时快照、fire 时才投递，归属悬空期无定义）；② 重复（`send_interval`
同一份快照每周期投一次 → 多个 actor 各持「合法」header，必然 double-free，且写
`owner_pid` 的是 poller 线程，违反「owner_pid 只由属主写」，见 §11 S3）。`timer_register`
（`lib/timer.c:126-150`，在注册 actor 的 cfunc 线程上跑，快照阶段读注册方堆）报错。
需要「定时后给别人用」，发 pid/符号让接收方自己创建资源。注册路径只有
send_after/send_interval 两条，快照注册后不可变、cancel/fire 结束即 free——S3 的封
口完整。

**禁止：资源消息走 selective receive（`b_recv_peek`）。**（R2 N1，实现前裁决。）
`b_recv_peek`（`src/builtin.c:38-69`）把 fragment 内容深拷进堆但**不弹出** fragment
——此刻 actor 堆一份 + 自己邮箱 fragment 一份，两份 owner==self，与 self-send 状态
同构且**不违反任何其它禁令**（B 合法回送 + A peek）。若 peek 出的拷贝成垃圾，§6.2
的 GC 兜底会把 payload 释放，而消息还在邮箱里排队——之后 recv 取出只剩 "closed"。
内存安全靠 `released` 兜住，但「排队中的消息被无关 GC 关闭」违反单归属直觉且不可
预期。源头拒绝：`b_recv_peek` 的 `val_deep_copy` 遇 `HEAP_RESOURCE` 报错（需要资源
消息的 actor 用普通 recv）。

**「广播同一个资源」不可表达**：第一次 send 之后归属已转走，第二次 send 在收口处报
错。fan-out 只能各发各的值。

**同一条消息里同一个资源出现两次**（`cons(r, r)`，或 spawn 闭包捕获两次）：
`frag_copy` **和 `val_deep_copy`（spawn 路径）两处**收口对同一 payload 指针的第二次
出现都报错（R2 N5：只在 frag_copy 去重会让「send 里 cons(r,r) 报错、spawn 里同名
捕获静默通过」变成同一禁令的两种待遇；即便漏网，payload 侧 `released` 也保证 dtor
只跑一次，双保险）。

**错误机制**（R2 N2，实现前写死）：以上所有收口违规（self-send、第二次 send、
owner 不符、timer 带资源、peek 带资源、重复 payload）一律 = **进程隔离错误**
（`PROC_DIE_ISOLATED` 同款机制，`src/vm.c:97-105`，divzero/cartype 先例），不是静默
nil/0/丢消息。`frag_copy` 返回 Val、`val_deep_copy` 返回 Val、`mbox_deliver` 返回
void、`timer_register` 失败返回 0 且 `timer_c_send_after` 静默 `val_int(0)`
（`lib/timer.c:158-160, 208-216`）——现状没有任何错误通道，不写死机制每个禁令都会
退化成静默丢数据的洞。

**LB_SEND 对已死目标静默丢弃不经 frag_copy**（`src/vm.c:172-175`）：对资源无害
（归属留发送方，发送方后续 GC/死亡会释放），与 Erlang 语义一致，见 §5.4。

## 5. 并发分析（这一节回答「GC 会不会跟另一边使用冲突」）

### 5.1 不变量

**不变量 H（堆单写者）**：每个 `Proc` 的堆只被「当前正在运行该 proc 的线程」读写。
GC 总在该线程上跑（`gc_collect` 由分配触发，而分配只发生在运行中）。代码里的证据：

- `mbox_pop`（`src/scheduler.c:252-253`）注释：「Caller guarantees p owns its
  execution context (no heap races)」；
- `src/vm.c:7-9` 头注释：「多 worker 并发跑不同 proc 时全局别名必然竞态」；
- `src/prof.c:19-24` 注释：`proc_die` 的 `free(p->mem)` 不会被 race（且采样在运行线程
  自身做，无跨线程堆读）；
- GC 只由 `proc_heap_alloc` / `proc_gc_drain` 在属主线程触发（`ta_inline.h:691-712`）；
  timer poller 只 `frag_copy` malloc 的快照，不碰任何堆。

**pid 单调不复用**（`src/scheduler.c:316` atomic_fetch_add、318-323「pids are never
reused」注释、324-331 越界 abort）—— `owner_pid` 不会指到「换过人的」actor，这是
`owner_pid` 方案的安全基石。

**唯一跨 actor 的面是邮箱**，其队列结构由 `target->mbox_lock` 保护（`mbox_deliver`
在锁内做「投递 + 唤醒」，保证一个 proc 至多一个线程在跑 —— Skynet 不变量）。

### 5.2 payload 的使用 vs GC

payload 只被「当前拥有者」使用（用户纪律）。拥有者的一切（模块操作、GC、send、
close）都在拥有者自己的线程上，串行发生。**不存在「GC 在一个线程上跑、使用在另一
个线程上跑」的情况**，所以这层没有冲突可言。

### 5.3 send 临界区

时间线（`send` 到「收方 recv」之间）：

```
T1（发送方线程）        T2（发送方 GC）      T3（接收方线程/GC）
frag_copy 读本堆 header      —                   —
锁内: 入队成功               —                   —
锁内: 自己堆 header 改 owner   —                   —
       fragment 各副本改 owner —                   —
入队、发布 ────────────────────────────────►  锁内入队
                                     recv: deep_copy → 本堆
                                     模块操作 / GC 全在本线程
```

- `frag_copy` 读的是发送方自己的堆 → 不变量 H 保证没有别的线程在动它；
- 发送方 GC 与发送本身同线程（同一个 proc 正在跑），不可能并发；
- 接收方 GC 看不到 fragment（GC 不扫邮箱），只看本堆里的 header；
- fragment 由 `mbox_lock` 发布，`mbox_pop` 在锁内取走，写读之间有 happens-before；
- 发送方堆里的旧 header 在入队成功的同一把锁内被改写成 `owner_pid = 接收方`，之后
  无论何时被自己的 GC 扫到都跳过，不碰 payload。

结论：**send 临界区内没有资源相关的共享可变状态，也就没有需要同步的东西**。需要
atomic 的窗口在「共享一个 header + 引用计数」的模型里存在，本设计把它消掉了。

### 5.4 垂死的接收方

- 投递时已死 → `mbox_deliver` 走 `free(frag)` 丢弃分支，归属留在发送方（§4）；
- 入队后、recv 前死掉 → fragment 留在邮箱里。`proc_die` 在 `mbox_lock` 内 drain
  fragment，扫出其中的 `HeapResource`，`owner_pid == 死者` 的跑 `resource_release`。
  与并发的发送方由同一把 `mbox_lock` 串行化；
- main 死亡清算的 grace 窗口内（§6.4），RUNNING proc 仍可 send 资源给已判死的 proc
  → `mbox_deliver` DEAD 分支 `free(frag)`、归属留发送方，发送方最终由
  proc_die/vm_free 扫到——无泄漏，无需额外处理。

### 5.5 发送方死亡

发送方堆里的旧 header 无人负责（`owner_pid != 自己`），`proc_die` 的堆扫描跳过它
——payload 已归接收方，不会被误释放。

## 6. 释放

四条路径（v1 只有三条，vm_free 从开放问题升级为必需项，见 §11 S5）：

1. **显式 `close()`**（模块 API，如 `buffer.close(b)`）：`resource_release(header)`。
2. **GC 兜底**：actor 活着但资源成垃圾 → 收集结束时线性扫**旧 fromspace**，取未被
   转发的 `HEAP_RESOURCE`，`owner_pid == 本进程` 则 `resource_release`。插入点在
   `free(old_mem)`（`src/gc.c:236`）**之前**，且必须在 swap **之前**把旧 `heap_ptr`
   存进局部变量（swap 实际在 `gc.c:214-221`，`heap_ptr` 被覆盖在 218；224-229 是栈拷
   贝段）；扫描范围 `[old_mem, old_mem + 旧 heap_ptr)`。扫法照抄 `gc_scan_tospace`
   （`src/gc.c:103-108` 的类型断言同款走法）：bump 分配无洞、对象 8 对齐，按
   `obj_size` 线性走一遍即可（`obj_size` 的 default abort 在 `gc.c:33-37`）。
3. **actor 死亡兜底**：`proc_die` 里 `free(p->mem)`（`src/scheduler.c:568`）**之前**线性
   扫活堆 `[0, heap_ptr)`，`owner_pid == 本进程` 的跑 `resource_release`；同时 drain
   邮箱 fragment（§5.4）。插入点必须在 DOWN 广播（`scheduler.c:527-545`，内含
   `proc_reserve_heap` 可能触发一次收集、移动堆对象）**之后**。`PROC_DEAD` 置位
   （429）早于 drain（546-561），与并发 deliver 的两种交错都封死。
4. **VM teardown 兜底**：`vm_free`（本体 `src/api.c:259`，procs[] 循环 280-296）对每个
   残余 proc 扫活堆 + drain fragment 跑 `resource_release`，与 `proc_die` 复用同一段
   扫描函数。**这一条是必需项**，且比 R1 说的更常见：main **正常**退出也不走
   `proc_die`（`src/vm.c:1020-1027`：入口帧返回时 main 分支只设
   `exit_val/entry_done`，只有 else 分支才调 proc_die），main 自己的堆、以及被
   entry_done + DRAIN_QUANTA（`scheduler.c:905-910`）排空后遭 vm_request_stop 遗弃的
   WAIT proc 全部只能靠 vm_free 收；main 异常死亡清算（`scheduler.c:473-520`，kill 点
   494）同样不走 proc_die。`vm_free` 在所有线程 join 之后跑（`api.c:264-266` 注释，
   join 证据 `scheduler.c:1001` + `811-814`），单线程无并发问题；retired 链上的 proc
   已在 proc_die 扫过且 mem 置 NULL，不会重复扫。**扫描必须覆盖 main 自己的堆**。
   附注：CLI（tavm）路径不调 vm_free（`src/main.c` 仅 662 的 wasm 重入调），进程退出
   靠 OS——「不泄漏」承诺的实际受益者是 wasm 重入与 embedder，对 CLI 只是 LSan 观感。

统一收尾：

```c
static void resource_release(HeapResource *r) {
    ResourcePayload *p = r->res;
    if (p->released)
        return;                       /* 副本已释放过：幂等 */
    p->released = 1;
    p->ptr && p->dtor(p->ptr);
    p->ptr = NULL;
}
```

`released` 在堆外 payload 上，所以 `cons(r, r)` 的第二份副本、`b_recv_peek` 的「堆
一份 + fragment 一份」、ping-pong 回送的 stale 副本都会命中幂等，不会 double-free。
dtor 的执行上下文全在属主线程（close / §6.2 / §6.3）或 join 后的单线程（§6.4）。

模块操作开头一条判空（不是安全机制，是防 NULL 崩溃的一条 if）：

```c
if (r->res->released) return ta_error("buffer is closed");
```

### 6.1 dtor 的执行上下文约束（写进 `docs/c-module.md`）

dtor 会在 GC 尾声（fromspace 扫描中）、`proc_die`、`vm_free` 里跑。约束：

- 不得分配 TA 值、不得重入 VM、不得再触发收集；
- 可以阻塞（`lib/process.c` 的 dtor 是 `waitpid`/`kill`，跑在 GC 里可接受，但明说）；
- `ta_resource_new` 断言调用不在 `p->ccall` 回调窗口内（三段非阻塞 cfunc 协议见
  `src/vm.c:284-294`，yield 路径 callback_arena_release「链上无活对象」
  `vm.c:318-324`；chunk 路由 `ta_inline.h:660-670`）。

## 7. 已决事项

1. **模块操作必须做 owner 检查**（R1 的开放问题 1，升级为必须）：资源分支开头
   `if (r->owner_pid != tls_current_proc->pid) return error`，一次 int 比较。与
   `frag_copy` 收口的校验同源、同样 NULL-safe。
2. **`vm_free` 扫描是必需项**（R1 的开放问题 2，移入 §6.4）。
3. **同一消息重复资源**：`frag_copy` 与 `val_deep_copy` 两处去重报错 + payload 侧
   `released` 双保险。
4. **语言层形态**：内建不透明类型 `Resource`（无构造子、不可 `match`、不可比较——
   `val_equal` 默认按 header 指针恒等，`src/val.c:466-469`，与规格一致）；打印成
   `<resource>`；不做 `resource?` 谓词。

## 8. 触点清单

| 位置 | 改动 |
|---|---|
| `ta.h` | `HEAP_RESOURCE`、`TAG_RESOURCE = 0xFFFC`（`TAG_LAST = TAG_CLOS_ID = 0xFFFB` 在 `ta.h:66`，同步更新；`val_is_float = t < TAG_FIRST \|\| t > TAG_LAST` 在 `ta_inline.h:38-42`——不改也判 false，但 TAG_LAST 语义应更新；NaN 装箱规范化前提见 `ta_inline.h:31-37`）、`HeapResource`、`ResourcePayload`、API 声明。`sizeof(HeapHeader) == 2`（`ta.h:123-127`） |
| `src/val.c` | boxing、打印（`<resource>`）、`val_deep_copy`（164，含 owner 校验 + payload 去重）、`val_calc_heap_size`、**`val_converge_copy`（`val.c:303`）**。注意：`val_converge_copy` 对**堆内对象按引用透传**（`val.c:364-368`：`!val_in_chunk → 保留原引用`），资源 Val 穿过回调收敛**不会**产生 header 副本，天然安全——需要加的只是可选的 owner 校验分支与「未知 in-chunk tag → nil」的防御。而 `val_deep_copy` 的 else→nil 分支（236-238）会把漏加的资源**静默变 nil**——所有 Val 遍历 switch 的 default 建议一律 `fatal`，不要 nil |
| `src/gc.c` | `obj_size`（default abort 在 33-37）、`gc_copy_val`、`gc_scan_tospace`（类型断言 `GC_ASSERT` 实际 `gc.c:103-108`）各一行；新增 fromspace 未转发扫描（§6.2）。注意 forwarding 指针写在 obj+2 起 8 字节（`gc.c:81-82`），覆盖 padding(2..4) + `owner_pid`(4..8) + **`res` 低 2 字节(8..10)**——被转发的 fromspace 副本只看 `FLAG_FORWARDED`，字段不再被读，注释点明（含 res） |
| `src/scheduler.c` | `mbox_deliver`（223-243）入队成功后改写归属，**需要第三次遍历 msg 树**（§4 实现前提）；`frag_calc_size`（函数体 `76-118`，103 是体内一行；switch 无 default 计数——漏分支 = fragment 缓冲区按小分配）与 `frag_copy`（124）加资源分支 + 收口校验（owner / self-send / 重复 payload，NULL-safe）；`proc_die` 扫活堆 + drain fragment（§6.3） |
| `src/builtin.c` | `b_recv_peek`（38-69，deep copy 在 49）遇资源报错（§4 peek 禁令）；`b_recv_peek`/commit 路径加测试 |
| `src/api.c` | 模块 API 实现；`vm_free` teardown 扫描（§6.4，覆盖 main 自己的堆） |
| `src/vm.c` | `LB_SPAWN`（145-167）改写插入点 [163, 165]；`LB_SEND`（169-176，DEAD 早退 172-175 不经 frag_copy，§5.4）；`PROC_DIE_ISOLATED`（97-105）作为收口违规的报错机制 |
| `lib/timer.c` | `timer_register`（126-150，注册线程上快照阶段）拒绝资源值；错误走进程隔离错误而非静默 0（158-160, 208-216）；注释里的 "b_send (src/builtin.c)" 是陈旧引用，顺手更新 |
| `docs/c-module.md` | §5「裸 int 句柄 + 槽表 + generation」整段退休，换成资源值一条路 + dtor 约束（§6.1） |
| `docs/ta-language-spec.md` | `Resource` 不透明类型 + 「资源不得 send 给自己、不得进定时消息、不得走 selective receive」三条限制 + 违规 = 进程隔离错误 |
| `lib/buffer.c` | 删表/锁，`Buffer { Buf(Resource) }`，dtor = free(data) |
| `lib/process.c` | 同上，dtor = 关子进程 |
| `lib/tls.c` | 同上，dtor = SSL_free/BIO_free（OpenSSL 本身不动） |
| `test/` | 正例：close 后操作报错、跨 actor 移交后可用、GC/退出/teardown 兜底不泄漏、`cons(r,r)` 双副本不 double-free、B 回送后 A 用普通 recv 可用；负例：send 给自己、第二次 send 同一资源、timer 消息带资源、peek 消息带资源、移交后旧属主再 send、spawn 闭包重复捕获同资源 |

## 9. 迁移顺序

1. **buffer** —— 最简单的验证品，改完立刻能对比删掉多少代码（预计 -150 行
   槽表/锁代码）；
2. **process** —— `fork/exec/waitpid` 本来就在锁外，改完锁整个消失；
3. **tls** —— OpenSSL 不动，只换胶水层；
4. **tcp fd**（后续，不在本次）：`Conn(Resource)`，dtor = `close(fd)`。

## 10. 与 BEAM / Lua 的对照

| | 资源形态 | 归属 | 释放 |
|---|---|---|---|
| BEAM resource term | 引用计数 term，跨进程共享 | controlling process（port）/ 无（resource） | refcount 归零 → destructor |
| Lua userdata | full userdata + `__gc` | 无 actor 模型 | GC finalizer（非确定），`__close` 可确定性 |
| 裸 int 句柄（现状） | int + 模块槽表 + 全局锁 | 无 | 手动 close，泄漏靠 gen/ABA 防呆 |
| **本设计** | 每持有者一份 header + 堆外共享 payload | send/spawn 移交，C 侧记 `owner_pid` | `close()` 确定性 + GC/死亡/teardown 三重兜底 |

BEAM 用引用计数解决「多个 actor 同时持有」的问题；本设计取消了多持有，因此
`refs`、atomic、锁全部不需要。

## 11. 评审记录

### R1 → v2（外部评审 GLM-5.3，逐条核对代码）

判定「骨架正确、不变量 H 成立、`mbox_deliver` 确实是投递收口」，找出 5 个严重缺陷，
v2 全部采纳：

| # | 缺陷 | v2 的修正 |
|---|---|---|
| S1 | v1 的 §4 只改写 fragment/子堆副本的归属，**发送方自己堆里的旧 header 从没被改写**——而 §5 的安全论证依赖它。结果：A send 后自己的 GC 把旧 header 当垃圾释放 → B 正在用的 payload 被 free（UAF + double-free） | §4：移交时属主线程同时改写「自己堆 header + fragment 每副本 + 子堆副本」三处 |
| S2 | `send(self, …)`：GC 不扫邮箱，发送方堆里的副本成垃圾被释放，邮箱 fragment 还持活引用 → UAF/double-free | §4：`frag_copy` 收口禁止 self-send |
| S3 | timer 是遗漏的第四条投递路径（poller 线程投递、延迟、`send_interval` 重复投递）→ 归属悬空期无定义、跨线程写 `owner_pid`、必然 double-free | §4：`timer_register` 快照阶段拒绝资源 |
| S4 | `closed` 每 header 一份，不提供跨副本幂等（`cons(r,r)`、`b_recv_peek` 在合法流里就会 double-free） | §3/§6：`released` 下沉到共享 payload；`frag_copy` 去重报错双保险 |
| S5 | main 先死的死亡清算（`scheduler.c:473-520`）不走 `proc_die`，`vm_free` 无扫描 → 「忘记 close 不泄漏」在最常见的程序形态下失效 | §6.4：`vm_free` 全量 teardown 扫描升级为第四条必需释放路径 |

### R2 → v3（外部复审 GLM-5.3，独立重核所有断言）

**总体**：S1–S5 修正方向全部成立；未发现新的严重级（UAF/double-free/泄漏/竞态）
缺陷；设计可以进入实现，带四条前置条件（本 v3 全部落实）。

- **S1 成立**，附三个实现前提（已写入 §4/§8）：mbox_deliver 需第三次遍历 msg 树；
  timer fire 的改写是 no-op（依赖 register 已拒）；三处改写顺序经逐路径推演无竞态
  （抢占/死亡/接收方死亡三种交错全部封死）。
- **S2 部分成立**：self-send 禁令不覆盖「B 回送 + A 的 selective-receive peek」等价
  路径，该状态的兜底是 S4 的 `released`——S2 与 S4 是耦合修复（已写入 §3/§4）。
- **S3 成立**：register 校验位置正确（注册 actor 的 cfunc 线程）；两个防御性缺口
  已补（NULL-safe 校验、错误机制不得静默 0）。
- **S4 成立**：24 字节账目属实（每资源一份，不是每副本）；dangling `res` 风险没有
  被低估（GC 扫描读 header 的 `owner_pid`，不读 payload）。
- **S5 成立**，且更常见：main **正常**退出也不走 proc_die，vm_free 是正常退出路径上
  的唯一兜底（已写入 §6.4）。
- **新问题（全部一般级，已落实）**：N1 peek 提前关闭排队资源 → §4 peek 禁令；
  N2 错误机制未定义 → §4「错误机制」段（进程隔离错误）；N3 校验 NULL-safe → §4；
  N4 forwarding 踩踏漏 `res` 低 2 字节 → §8；N5 去重缺 spawn 路径 → §4/§7.3；
  N6 LB_SEND 死目标静默丢弃 → §5.4 备忘。
- **事实核对**：20 项断言，15 项属实、5 项行号不准（全部已按本轮实测校正：`gc.c:236`
  精确；swap `214-221`；`val_converge_copy` `val.c:303` 精确且堆对象按引用透传；
  `frag_calc_size` `76-118`；`TAG_LAST` `ta.h:66`；forwarding `gc.c:81-82`；pid
  `316-331`；根集 `gc.c:194-203`；`GC_ASSERT` `103-108`；DOWN `527-545`；main 清算
  `473-520`；vm_free `api.c:259`；peek `builtin.c:38-69`；「b_send」名称已过时，
  实际调用点 4 个）。`val_converge_copy` 的透传语义是本轮新发现的简化——资源穿过
  回调收敛天然安全，不需要强制分支。
- **三个裁决全部建议同意**（禁 self-send / 禁资源进 timer / payload 24B 永不回收）：
  替代方案（邮箱纳入 GC 根集、注册即移交、payload 注册表）复杂度与收益完全不成比
  例，或直接违反地基不变量。