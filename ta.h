#ifndef TA_H
#define TA_H

#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * Value representation — NaN-boxing (64-bit)
 * ============================================================ */

typedef uint64_t Val;

/* Extract the 16-bit tag from a NaN-boxed value — see ta_inline.h */

/* NaN-boxing tags: bits [63:48] of the 64-bit value.
 *
 * All tags live in the IEEE 754 "exponent all-ones, sign=1" zone — the
 * negative NaN range — so no finite double can collide with a tag: a finite
 * double has exponent <= 0x7FE, giving a top 16 bits of at most 0xFFEF.
 * TAG_FIRST starts at 0xFFF1 so that -Inf (exactly 0xFFF0000000000000,
 * top 16 bits 0xFFF0) also stays outside the tag range and round-trips as a
 * plain float. The only remaining colliders are NaN bit patterns, which are
 * canonicalized at box time (val_float / val_from_double, ta_inline.h).
 *
 * Normal doubles are stored as-is. Non-double types use the high
 * 16 bits as a tag; the low 48 bits carry payload.              */
#define TAG_FIRST 0xFFF1
#define TAG_INT 0xFFF1
#define TAG_NIL 0xFFF2
#define TAG_TRUE 0xFFF3
#define TAG_FALSE 0xFFF4
#define TAG_SYM 0xFFF5
#define TAG_PAIR 0xFFF6
#define TAG_PID 0xFFF7
#define TAG_CLOS 0xFFF8
#define TAG_STRING 0xFFF9
#define TAG_BYTES 0xFFFA
#define TAG_CLOS_ID 0xFFFB /* direct fn_id (no heap alloc, nfree=0) */
#define TAG_LAST TAG_CLOS_ID

/* 载荷读写：布局就是上方 TAG 注释文档化的公开约定（高 16 tag / 低 48 payload）。
 * TA 的 val_payload48/box_tag_payload 未导出，而 TAG_CLOS_ID/装载器自定的
 * TAG_NATIVE 的载荷就是普通整数，这里按公开约定读写，pair/string/closure
 * 一律走 val_get_car 等正式访问器，不碰指针。解释器（vm.c）与装载器
 * （main.c，字符串常量直建 HeapString）共用。 */
static inline uint64_t lpayload(Val v) { return v & 0x0000FFFFFFFFFFFFULL; }
static inline Val lbox(uint16_t tag, uint64_t payload) {
    return ((uint64_t)tag << 48) | (payload & 0x0000FFFFFFFFFFFFULL);
}

/* Canonical quiet NaN stored whenever a double NaN is boxed: its top byte is
 * 0x7F, so it sits in the float region and prints as "nan" (%g). NaN payload
 * bits are not preserved — the language never constructs NaNs with payloads,
 * and any NaN reaching the VM (0.0 / 0.0) is observably just "nan". */
#define VAL_CANON_NAN 0x7FF8000000000000ULL

/* Heap object types (stored in HeapHeader.type) */
#define HEAP_PAIR 1
#define HEAP_CLOS 2
#define HEAP_STRING 3
#define HEAP_BYTES 4

/* HeapHeader.flags bit: set while a GC copy has relocated the object; the
 * slot right after the header then holds the new address (gc_copy_obj,
 * val_converge_copy). Only observed inside a collection — fromspace and
 * chunk objects are dead afterwards. */
#define FLAG_FORWARDED 0x01

/* ---- Actor memory block layout (tuning knobs) ----
 *
 * One contiguous block per actor:
 *
 *     low addr → [ TA_PROC_CHUNK0 | heap ↑ ... ↓ stack ] ← high addr
 *
 * TA_PROC_CHUNK0 reserves a slice at the bottom of every block for the
 * C-callback chunk arena (gc issue #160): while a C module runs, its
 * allocations bump-allocate here instead of the managed heap, so the
 * managed heap stays frozen and nothing needs rooting. Overflow past the
 * slice falls back to malloc'd chunks. The region is otherwise untouched,
 * so a small default is free — tune by observing how often the overflow
 * path fires. */
#ifndef TA_PROC_CHUNK0
#define TA_PROC_CHUNK0 256
#endif
/* Usable heap+stack (excluding the chunk slice) an actor grows to when its
 * first heap object is allocated. Keep the initial arena small; stack and
 * heap growth is handled on demand by the arena helpers below. */
#ifndef TA_PROC_HEAP0
#define TA_PROC_HEAP0 512
#endif

#define MAX_PROCS (1024 * 1024)

/* Proc.recv_deadline_ms sentinel: the deadline fired while the proc was
 * blocked in recv_after (BUILTIN_RECV_AFTER) — the timeout won, so the
 * builtin must return nil and leave the mailbox untouched (messages arriving
 * after expiry belong to the next receive). */
#define RECV_AFTER_EXPIRED (-2)

/* ============================================================
 * Heap object structures
 * ============================================================ */

typedef struct {
    uint8_t type;
    uint8_t flags;
} HeapHeader;

typedef struct {
    HeapHeader hdr;
    Val car, cdr;
} HeapPair;

typedef struct {
    HeapHeader hdr;
    int entry; /* bytecode offset */
    int nfree;
    Val free[]; /* variable-length: captured values */
} HeapClosure;

typedef struct {
    HeapHeader hdr;
    int len;
    char data[]; /* variable-length, NUL-terminated */
} HeapString;

typedef struct {
    HeapHeader hdr;
    int len;
    uint8_t data[]; /* variable-length */
} HeapBytes;

/* Message fragment (Task 2 will wire this into mailbox) */
typedef struct MsgFragment {
    struct MsgFragment *next;
    int size;
    Val root;
    uint8_t data[];
} MsgFragment;

/* ============================================================
 * Process & Scheduler
 * ============================================================ */

typedef struct VM VM; /* forward declaration for TaFunc */

/* Module function descriptor */
typedef struct {
    const char *name;
    Val (*fn)(VM *vm, Val *args, int nargs);
    int nargs;
} TaFunc;

typedef enum { PROC_RUNNING, PROC_WAIT_RECV, PROC_WAIT_IO, PROC_DEAD } ProcState;

#define MAX_TOK_VECS 32

/* One malloc'd chunk of the gate-closed callback arena (see Proc.gc_ck_head).
 * The first chunk is not malloc'd — it is the TA_PROC_CHUNK0 slice at the
 * bottom of the proc's `mem` block, and its data pointer is always derived
 * from p->mem so it survives arena growth/swaps. */
typedef struct TaChunk {
    struct TaChunk *next;
    int used;
    int cap;
} TaChunk;

/* ---- 冷扩展块：只有真的用到这些子系统的 actor 才分配 ----
 *
 * 判据是「1M 个 actor 里，有几个真的会碰这个字段？」。一个只 spawn / send /
 * recv / 算术的 actor，不该为 token-vector 注册表、C 回调 arena 记账、外部 fd
 * 等待现场、selective-receive 游标付内存 —— 它们原来内联在 Proc 里，占
 * sizeof(Proc) 584 B 中的 288 B。挪进这块懒分配的冷块后，热核 272 B/actor，
 * 而典型 actor 一个字节都不碰这里。
 *
 * 铁律只有一条：**这块内存独占于它的 actor，只有该 actor 自己的线程会创建
 * 或写入它**（proc_cold() 及其调用者：C 回调的 chunk 分配、make_tok_vec、
 * peek）。旁观者（GC、scheduler 投递 DOWN、poller 扫描、vm_free）一律直接读
 * p->cold 并容忍 NULL —— 它们不是冷块的所有者，不替别人分配内存。
 *
 * 冷/热的界线：**冷块放「可选子系统的大块表 / 缓冲区」，不放零零碎碎的小标量**。
 * 于是 watcher 数组（别的 actor 会写，见下）、wait_fd / wait_events、
 * last_errno 都留在热核 —— 它们都被外部（monitor 持有者、poller、C 模块）
 * 在大量散点上直接读写，挪进冷块只会换来一堆 NULL 判断和 proc_cold 噪声，省不下
 * 几个字节。
 *
 * 反例：monitor 的 watcher 数组由「别的 actor」在 procs_lock 下 realloc，
 * 不满足独占性 —— 更不能进冷块（否则两个线程会同时看到 p->cold == NULL 而
 * 各自 calloc 一块，一块泄漏、watcher 分裂）。
 *
 * calloc 归零，与 Proc 自身靠 calloc 归零同一套语义；唯一非零初值是
 * gc_ck_head.cap（chunk0 是 mem 底部那块切片，由 proc_cold() 补上）。 */
typedef struct ProcCold {
    /* token-vector registry (vm.make_tok_vec / vm.tok_type / vm.tok_val /
     * vm.free_tok_vec). Per-proc since #121: the old process-wide table in
     * api.c was not thread-safe and leaked on proc death. Ids are opaque
     * to TA and only meaningful within the owning proc. Elements are
     * TokVec* (defined in api.c), stored as void*. */
    void *tok_vecs[MAX_TOK_VECS];

    /* Chunk arena 记账（issue #160）：只在 in_ccall 窗口内有意义。首块的
     * data 仍是 mem 底部的 TA_PROC_CHUNK0 切片（指针由 p->mem 推导，所以
     * 记账搬来这里不影响它的生存性）；overflow 链接 malloc'd TaChunk。 */
    TaChunk gc_ck_head; /* inline first chunk (data = p->mem, cap fixed) */
    TaChunk *gc_ck_cur; /* chunk being bumped: &gc_ck_head or a malloc'd one */
    int gc_ck_total;    /* U watermark */

    /* selective-receive scan cursor: index of the next mailbox fragment
     * to try during an in-progress (receive ...). Reset to 0 by
     * recv_commit (matched) — preserved across a block so resumed
     * scans only inspect newly-arrived messages. 普通 recv 不碰它。 */
    int peek_index;
} ProcCold;

typedef struct Proc {
    /* ---- 执行核心：每个 actor 都真的会用到的字段（272 B）----
     * 字段顺序就是打包顺序：4 字节字段紧随 8 字节槽，窗口标志合成一个
     * word，于是 4 字节字段成对、没有 padding 洞（加字段时保持这个分块）。 */
    int pid;
    atomic_int state;

    /* execution context */
    int sp;          /* stack top offset (grows downward from mem end);
                      * 解释器深度（run_proc 的 vsp 寄存器）的发布视图：
                      * GC 根集扫描界（gc.c）与阻塞边界的恢复源 */
    int stack_floor; /* deepest frame ever reserved (same negative-index
                      * convention as sp). The heap-side fit checks measure
                      * against min(sp, stack_floor): a reserved-but-not-yet-
                      * pushed frame region must stay clear of the heap,
                      * because the pushes that fill it happen later
                      * (proc_stack_lo in ta_inline.h). */
    int reductions;
    int mem_size;
    int heap_ptr;   /* heap top offset (grows upward; starts at TA_PROC_CHUNK0) */
    int mbox_count; /* number of queued messages */
    int has_acc;    /* recv 阻塞不读 acc（恒 0），ccall 重入置 1 */

    /* monitor watchers — 留在热核的原因见 ProcCold 顶部的「铁律」：这张数组
     * 由别的 actor（monitor 持有者）在 procs_lock 下 realloc，不是本 actor
     * 独占的，因此不能住进懒分配的冷块。watcher_refs 与 watchers 平行，
     * 同下标一一对应（两个指针在下面的 8 字节槽区）。 */
    int watcher_count, watcher_cap;

    /* ---- 解释器恢复态："这个 proc 跑到哪了"（run_proc 的唯一入口约定）----
     *
     * proc_frame_init（建帧）发布首跑恢复点，阻塞/让出边界（recv / ccall
     * yield）发布恢复点，run_proc 因此只有「恢复」一条路径。字段内联在 Proc
     * 里而不是单独一块：proc_new 的 calloc 顺带归零，省一次分配与一个 NULL
     * 分支；1M actor RSS 门实测，内联比原来的 pid 侧表形态低 ~7 MB（独立堆块
     * 在 malloc 尺寸类里比 Proc 多出的字段还贵）。
     *
     * pc/base/cbase 是「词流/帧」的索引形态（int：域不超过 2^31，与 sp 同宽）；
     * 栈深不外存 —— 就是上面的 Proc.sp 本身（负数偏移）。发布者把它写成「重执行
     * 本条指令时要的栈深」，run_proc 直接拿它物化 vsp。无需保存实参数 —— RET
     * 指令自带操作数 k（= 本函数实参数，编译期静态），沿 base+k+1 定位 cont
     * 三槽（见 docs/backend.md）。 */
    int pc, base, cbase;

    int gc_trigger; /* heap_ptr above which an allocation requests collection */
    int gc_to_cap;  /* malloc'd capacity of the gc_to buffer */
    int gc_to_size;
    /* GC stress knob (TA_GC_STRESS=N): allocation countdown until the next
     * collection request on this proc (sets gc_pending, honoured like any
     * other request once the gate is open). Only touched when the knob is on,
     * but it sits in the hot core because proc_heap_alloc reads it. */
    int gc_stress_cnt;
    /* net module (lib/net.c): last hard-failure errno of this proc, read back
     * by net.errno(). Per-proc, not thread-local: a proc that yields on EAGAIN
     * may resume on a different worker thread, while a proc runs on at most one
     * worker at a time — so this field needs no lock and always belongs to the
     * right actor. Zeroed by proc_new. */
    int last_errno;

    /* I/O wait: wait_fd/wait_events 由 vm_watch_fd 设（-1 = 只等死线，无 fd）。
     * 留在热核：poller 与 proc_die 对每个阻塞 actor 都直接读它，挪进冷块只会
     * 换来一堆 NULL 判断，省不下字节。wait_next / wait_generation 是 wait
     * 链表的公共部分，同理。 */
    unsigned wait_generation;
    int wait_fd;
    short wait_events;              /* POLLIN or POLLOUT */
    unsigned short wait_registered; /* 0/1 — lives in the pad after wait_events */

    /* window flags: 0/1 — 全是「这一趟执行/这个窗口开着吗」，合成一个 word。
     * vm_yield()/vm_die() 设前两个（do_ccall 在 C 函数返回时读）；in_ccall 是
     * C 模块回调窗口 —— 一个专门的窗口标志，NOT the gc gate：gate 在 VM 内部
     * 也会关（recv/spawn 的 val_deep_copy、DOWN 走查），那些结果属于堆。 */
    unsigned yield_requested : 1;
    unsigned die_requested : 1;
    unsigned in_ccall : 1;       /* heap allocs route to the chunk arena (above) */
    unsigned gc_ck_converge : 1; /* copy-out in progress: heap allocs bypass chunks, no GC */
    unsigned gc_ck_promote : 1;  /* a converge collection is running: chunk objects copy */

    int gc_gate : 31; /* nesting depth of not-gc-safe regions; 0 = safe to collect */
    unsigned gc_pending : 1;

    /* ---- 8 字节槽 ---- */
    ProcCold *cold; /* 懒分配的冷扩展块（见上）。NULL = 这个 actor 还没用到
                     * 任何冷特性；创建只在 actor 自己的执行路径上发生。 */
    uint8_t *mem;
    MsgFragment *mbox_frag_head; /* fragment list head */
    MsgFragment *mbox_frag_tail; /* fragment list tail */
    int *watchers;               /* monitor 数组（见上；热核，非本 actor 独占） */
    Val *watcher_refs;           /* 与 watchers 平行，同下标 = 该 watcher 的 ref */
    uint8_t *gc_to;
    struct Proc *wait_next;
    struct Proc *next_retired; /* retired-proc free-list linkage (proc_die → vm_free) */
    Val die_reason;            /* crash reason symbol (immediate value, GC-invisible) */
    Val acc; /* 末参/结果交接槽：do_builtin/do_ccall 经它传参（CALL 语义：
              * 末参在 acc，结果写回 acc），阻塞时也存这 */
    uint64_t prof_last; /* --profile：上一次采样时刻。放这不放 run_proc 局部——
                         * 跨热循环活跃的局部量会挤压解释器循环的寄存器分配
                         * （实测 +4%），与 pc/acc 同生命周期才对。 */
    atomic_int_fast64_t wait_deadline_ms;
    atomic_int_fast64_t recv_deadline_ms; /* recv_after(ms): monotonic-ms deadline.
                                             < -1 (RECV_AFTER_EXPIRED): deadline fired
                                             while blocked — opcode returns nil without
                                             touching the mailbox; -1: disarmed (ms
                                             operand still on stack); >= 0: armed —
                                             scheduler wakes the proc once it passes */

    /* mailbox — heap-fragment message queue (thread-safe).
     * Messages live in malloc'd MsgFragment nodes OUTSIDE the process
     * heap, so send/recv never touch the target heap (GC-race free).
     * gc.c's mailbox scan is a no-op: fragments are malloc'd, hence
     * outside fromspace, so gc_copy_val() returns early. */
    pthread_mutex_t mbox_lock;
} Proc;

/* 冷块的唯一创建入口：只在 actor 自己的执行路径上调用（见 ProcCold 的纪律）。
 * 返回的指针永远有效（本函数内 OOM 是 fatal），调用者不必判空。 */
ProcCold *proc_cold(Proc *p);

/* 释放冷块及其内部所有 malloc（token-vector 表、C 回调 chunk 链）并置
 * p->cold = NULL。允许 p->cold == NULL（常见情形，直接 no-op）。只在确认没有
 * 别的线程能再碰这个 proc 时调用（vm_free）。热核里的 watcher 数组不在这里，
 * 由 vm_free 直接 free。 */
void proc_cold_free(Proc *p);
/* ---- actor 原语与调度 ----
 *
 * spawn/send/recv/self 编译为 LOP_BUILTIN：编译期固化整数 id，运行期
 * switch 派发（CALL 只服务 VM 自己的函数，cfunc 走 TAG_NATIVE 的 C
 * 模块协议 —— 分层：opcode / op_call / op_builtin / cfunc）。recv 可能
 * 阻塞，解释器状态按 proc 存活期存在 Proc 的恢复态字段里；阻塞时 pc 不前进，
 * 唤醒后重执行本条指令。 */

/* 图像格式版本（与 bytecode.ta write_bc 的头词严格一致）：v3 = RET 双字
 * 指令（操作数 k = 本函数实参数，帧尾 cont 三槽沿 base+k+1 定位；无 cargs
 * 寄存器）。v2 = 显式 continuation 协议但 RET 单字 + VM 侧 cargs 寄存器，
 * v1 = 旧双栈布局 —— 均装载即拒绝（main.c parse_unit 的版本门）。 */
#define TA_BC_VERSION 3

/* ---- opcode（与 compile.ta 严格一致；解释器 vm.c + 装载器 main.c 共用） ---- */
enum {
    LOP_CONST = 0,
    LOP_LOAD,
    LOP_STORE,
    LOP_LOADF,
    LOP_PUSH,
    LOP_ADD,
    LOP_SUB,
    LOP_MUL,
    LOP_DIV,
    LOP_MOD,
    LOP_LT,
    LOP_LE,
    LOP_GT,
    LOP_GE,
    LOP_EQ,
    LOP_PAIRP,
    LOP_SYMP,
    LOP_CONS,
    LOP_CAR,
    LOP_CDR,
    LOP_JIF,
    LOP_JUMP,
    LOP_CALL,
    LOP_TCALL,
    LOP_RET,
    LOP_MAKE_CLOSURE,
    LOP_GLOBAL,
    LOP_RESERVE,
    LOP_BUILTIN,
    /* 融合指令：x + PUSH 一条发完（实参传递形态，compile.ta fuse pass
     * 生成，两边同步加）。操作数与原指令一致，语义 = 原指令 + proc_push。 */
    LOP_LOADP,
    LOP_CONSTP,
    LOP_GLOBP,
    /* 融合比较+条件跳转：a = 栈顶（左操作数），b = acc（右操作数），弹 a，
     * 真跳 W[pc+1] / 假跳 W[pc+2]（JIF 同款双目标）。compile.ta fuse pass
     * 对 if <cmp> 形态生成（IIfP），语义 = IOp(cmp) + JIF，两边同步加。 */
    LOP_JIF_LT,
    LOP_JIF_LE,
    LOP_JIF_GT,
    LOP_JIF_GE,
    LOP_JIF_EQ,
    /* 立即数操作数形态（lib/bootstrap/compile.ta 发）：右操作数是指令内嵌
     * 的 int 立即数，不再经过量池。与源指令同语义，只是省掉一次
     * g_consts 依赖 Load——真正收益是把它身后的跳转/算术并成一条，少一
     * 次派发。词宽 = long（64 位），任意 TA int 可内嵌，无范围回退。
     * 只留实测流量最集中的四条：算术 ADD/SUB、比较 EQ、const 内嵌；其余形态
     * （MUL/DIV/MOD_I、JIF_LT/LE/GT/GE_I、CONSTP_I）在基准与真实负载里的
     * 流量占比都不到 8%，不值得各自的 opcode 面积（数据见 PR 说明）。
     * 与 compile.ta 的 OP_* 常量严格镜像，两边必须同步改。 */
    LOP_CONST_I, /* = OP_CONST_I */
    LOP_ADD_I,
    LOP_SUB_I,
    LOP_JIF_EQ_I, /* JIF_EQ 的立即数版 */
    LOP_COUNT
};

#define MAX_CFUNCS 128

/* Stringify a sanitizer tag macro (TA_MOD_TAG=tsan → "tsan") for use in
 * module paths: sanitizer builds load lib/http_tsan.dylib etc. */
#ifdef TA_MOD_TAG
#define TA_MOD_TAG_STR_(x) #x
#define TA_MOD_TAG_STR(x) TA_MOD_TAG_STR_(x)
#else
#define TA_MOD_TAG_STR(x) ""
#endif

/* Thread-local current process — set by the host run loop (tavm run_proc)
 * while a proc executes. */
extern __thread Proc *tls_current_proc;

struct VM {
    Proc **procs;
    int procs_count, procs_cap;
    Proc *retired; /* dead procs deferred to vm_free (proc_die list) */

    int *runq; /* ready queue (pid array, circular buffer) */
    int rq_head, rq_tail, rq_cap;
    atomic_int rq_count;

    atomic_int next_pid;
    atomic_int next_ref; /* monitor ref counter */

    /* ---- .tabc 图像态（装载侧 main.c 装载期写入，run 期间只读；vm_free
     * 释放）。此前经 vm.c 的 extern“装载接缝”消费的五个量，收进 VM 结构
     * 体：图像生命周期 = VM 生命周期，所有权单一。 ---- */
    long *words;                        /* 整数词流 */
    long *fn_entry, *fn_args, *fn_maxd; /* 函数表：入口偏移 / 参数个数 / 最大栈深 */
    Val *g_consts; /* 全局常量表（#278）：字符串 payload malloc 直建、永不搬移 */
    long nconsts;  /* g_consts 长度（vm_free 释放字符串 payload 用） */
    int fn_count;  /* 图内 fn 数（= 装载期 nfns；prof.c 按 [0,fn_count) 枚举） */
    int main_pid;  /* pid of main() process; -1 if none */
    atomic_int main_dead;    /* set when main() exits — triggers shutdown */
    atomic_int main_crashed; /* set when main() dies abnormally (reason !=
                                nil) — makes the host exit non-zero */
    /* entry 生命周期：run_proc 在 entry RET 时置 entry_done + exit_val，
     * worker_loop 据此收场、main 打印（与 main_pid 同法：随 vm_new 归零）。
     * 原子：多 worker 下由一个 worker 写、其余 worker 读。 */
    atomic_int entry_done;
    Val exit_val;

    /* Per-fn_id name table (NULL entry = anonymous fn, fallback
     * "fn#<id>"). Filled by the host at image load (tavm
     * install_frame_hooks) — used by prof.c / crash report. */
    char **fn_names;
    int fn_names_count, fn_names_cap;

    /* 图像特定的栈行走（CRASH 报告 / profiler 的共享路径经 vm_walk_stack
     * 分发）。NULL = 图像未装载（vm_walk_stack 返回 depth 0）。tavm 装载
     * .tabc 时注册自己的：调用帧在 Proc 栈上，返回的 fid 索引该图
     * 的 fn 表（与上面 fn_names 的下标对齐——tavm 装载 .tabc 时同步填充）。 */
    int (*walk_stack)(const VM *vm, const Proc *p, int *out, int max_depth);

    /* symbol table — appended at load time AND interned on worker
     * threads at runtime (str.to_sym, DOWN payloads); guarded by
     * sym_lock */
    char **symbols;
    int sym_count, sym_cap;
    /* 名字哈希（开放寻址，条目存下标；-1 空）：intern 的 O(1) 路径。
     * 编译器工作负载逐 token intern，线性 strcmp 扫描曾占编译期 ~90%。
     * 增长走「新建-替换-退役」——读者持有的旧表始终完整有效。 */
    int32_t *sym_hash;
    int sym_hash_cap; /* 2 的幂；0 = 未建（OOM 退化回线性扫描） */

    /* C function registry */
    struct {
        char *name;
        Val (*fn)(VM *vm, Val *args, int nargs);
        int nargs;
    } cfuncs[MAX_CFUNCS];
    int cfunc_count;
    /* 同上：cfunc 按名解析的 O(1) 路径（每次 CCALL 都要解析）。重名保
     * 首见，与线性扫描 first-match 同语义。MAX_CFUNCS 有界，建表后不再
     * 扩容。 */
    int32_t *cfunc_hash;
    int cfunc_hash_cap; /* 2 的幂；0 = 未建 */

    /* Module registry */
    TaFunc **mod_funcs; /* per-module function arrays */
    int *mod_nfuncs;    /* per-module function counts */
    char **mod_names;   /* module names */
    int mod_count, mod_cap;
    /* Prefix of the registry holding the statically registered C modules of
     * host_init（num/vm；net/timer/cov 已下沉为 dylib-backed 词条，
     * 见 api.c is_builtin_module）. Frozen by vm_seal_host_modules;
     * vm.is_builtin_module answers only about this prefix, so a lazy dylib
     * that dlopens later cannot change the answer (see api.c). */
    int host_mod_count;

    /* Threading infrastructure */
    atomic_int active_procs;
    atomic_int recv_armed; /* procs with an armed recv_after() deadline;
                              lets deadline scans skip entirely when 0 */
    pthread_mutex_t rq_lock;
    pthread_cond_t rq_cond;
    pthread_mutex_t procs_lock; /* protects vm->procs[] access */
    pthread_mutex_t wait_lock;  /* protects event-driven wait list */
    atomic_int wait_count;      /* size of the wait list; mirrors wait_head
                                   length so O(1) checks replace O(n) walks
                                   (sched idle loops poll this) */
    Proc *wait_head;

    /* timer 模块（lib/timer.dylib）向 poller 的反向注册：懒 dylib 无法被
     * core 静态调用，vm_load_self 时装入。NULL = 本进程未引用 timer.*
     * （poller 跳过死线扫描）。签名与原 timer_next_deadline_ms /
     * timer_fire_expired 一致（-1 = 无挂起死线）。 */
    int64_t (*timer_deadline_hook)(VM *vm);
    void (*timer_fire_hook)(VM *vm, int64_t now_ms);

    pthread_mutex_t sym_lock; /* protects vm->symbols/sym_count/sym_cap
                               * (interning takes the lock) */

    /* Buffers displaced by realloc while other threads may still hold
     * previously published pointers into them (words, fn_names,
     * symbols — see tavm link_unit / vm_intern_symbol). The .tabc
     * image tables (vm->words / fn_entry / fn_args / fn_maxd /
     * g_consts) are written once at load time, before any Proc runs,
     * and stay owned by the VM struct (freed in vm_free).
     * Append-only, so stale pointers stay semantically valid; the old
     * allocations are kept alive until vm_free instead of being freed
     * by realloc. */
    void **retired_bufs;
    int retired_count, retired_cap;
    pthread_mutex_t retired_lock; /* retired_bufs is touched under
                                   * sym_lock (interns) AND procs_lock
                                   * (module append) — needs its own */
    /* 收场（退出）标志。写它的地方只有一处：scheduler.c 的
     * vm_request_stop()（置 stop + broadcast rq_cond 唤醒所有 worker +
     * 唤醒 io poller 退出 poll）。谁先满足收场条件谁就调它，其余 worker
     * 每轮开头读 stop 退出 —— 「单一决策者 + 广播」，没有第二套语义。 */
    atomic_int stop;
    /* 忙窗口计数：已经出队、正在跑某个 proc 的 worker 数。收场判据与死锁
     * 判据都需要它 —— 「runq 空」不等于「没人有活干」，别的 worker 手里的
     * proc 还没跑完时 rq_count 就是 0。worker 出队前加、跑完后减，封住
     * 「出队与跑完之间」这个假空窗。 */
    atomic_int busy_workers;
    /* entry 终止后的排空片计数（上限见 scheduler.c 的 DRAIN_QUANTA）：
     * 让「已投递、已入队」的消息跑完再收场，但排空必须有界（无限自旋的
     * actor 会永远再入队）。语义是「全局还能再跑几片」，所以必须是 VM 级
     * 共享计数 —— 老宿主实现（main.c:357 的 sched 局部 static）每 worker
     * 一份，多 worker 下各自数到上限 = 收场提前 N 倍。 */
    atomic_int drain;
    /* worker 池大小。默认 1（vm_new 写死 1，宿主按 NWORKERS 覆盖）：
     * 单线程是语义基准，池要显式开（mt-revival.md §7 开放问题#2）。
     * vm_run 里 nworkers 个 worker = 本线程 + 另建 nworkers-1 条线程，
     * <=1 时一条都不建 —— 单线程与池不是两条分支，只是池大小为 1。 */
    int nworkers;

    /* I/O poller wake pipe (multi-thread mode only). An arm site writes a
     * byte so an io_poller already blocked in poll() with the lazy 100ms
     * cap re-scans and adopts a newly armed recv_after/wait deadline
     * immediately instead of waiting out the cap. Both ends are -1 in
     * single-thread mode, where the run loop re-scans its own deadlines
     * before polling and no wake is needed. */
    int wake_pipe_r, wake_pipe_w;

    /* io poller 线程句柄（vm_poller_start/stop 管理） */
    pthread_t io_thread;
    int io_thread_started;

    /* Sampling profiler (src/prof.c) — prof_on set once before the run loop
     * starts and never changed during it, so the run loop reads a plain int. */
    int prof_on;
    struct ProfState *prof;

    /* --trace N 的剩余轨迹条数：CLI 在 host_init 后写入，解释器 TRACE 读。*/
    long trace_left;
};

/* ============================================================
 * Bytecode instruction set
 * ============================================================ */

typedef enum {
    /* stack */
    OP_PUSH_NIL,
    OP_PUSH_TRUE,
    OP_PUSH_FALSE,
    OP_PUSH_INT8,   /* i8 */
    OP_PUSH_INT,    /* i64 (8 bytes) */
    OP_PUSH_SYM,    /* idx (4 bytes) */
    OP_PUSH_STRING, /* len(4), data(len) */

    /* local variables */
    OP_LOAD,  /* offset */
    OP_STORE, /* offset */

    /* pair */
    OP_CONS,
    OP_CAR,
    OP_CDR,

    /* arithmetic */
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,

    /* comparison */
    OP_EQ,
    OP_LT,
    OP_LE,

    /* type tests */
    OP_IS_NIL,
    OP_IS_PAIR,
    OP_IS_INT,
    OP_IS_STRING,
    OP_IS_BYTES,
    OP_IS_PID,

    /* control flow */
    OP_JUMP,          /* addr */
    OP_JUMP_IF_FALSE, /* addr */
    OP_POP,
    OP_DUP,

    /* functions */
    OP_CLOSURE,   /* fn_id, nfree, [offset...] */
    OP_CALL,      /* nargs */
    OP_TAIL_CALL, /* nargs */
    OP_RET,

    /* actor primitives — moved to OP_BUILTIN (see builtin_table below).
     * The nine numbers they used are kept as reserved slots so no surviving
     * opcode number moves.  Nothing emits them any more; if one ever reaches
     * the dispatch loop it falls through to the unknown-opcode report, the
     * same arm an out-of-range opcode takes. */
    OP_RESERVED_SPAWN,       /* 34, was OP_SPAWN */
    OP_RESERVED_SPAWN_MAIN,  /* 35, was OP_SPAWN_MAIN */
    OP_RESERVED_SPAWN_CLOS,  /* 36, was OP_SPAWN_CLOS */
    OP_RESERVED_SEND,        /* 37, was OP_SEND */
    OP_RESERVED_RECV,        /* 38, was OP_RECV */
    OP_RESERVED_RECV_PEEK,   /* 39, was OP_RECV_PEEK */
    OP_RESERVED_RECV_COMMIT, /* 40, was OP_RECV_COMMIT */
    OP_SELF,                 /* 41 — the one actor primitive that stays an opcode */
    OP_RESERVED_MONITOR,     /* 42, was OP_MONITOR */

    /* built-in */
    OP_HALT,

    /* Numbering is continuous from OP_ENTER to OP_BUILTIN. NOTE: this
     * enum is legacy — nothing in the C code emits or dispatches it any
     * more: the interpreter runs the LOP_* enum below, the
     * instruction-length table is src/main.c's g_optlen[LOP_COUNT], and
     * the bytecode constants the compiler emits live in
     * lib/bootstrap/bytecode.ta. Values are spelled out to keep the
     * historical numbering stable.
     *
     * There are no pattern-matching opcodes: the compiler lowers match /
     * receive patterns to generic tests (OP_EQ, OP_IS_NIL, OP_IS_PAIR) plus
     * OP_JUMP_IF_FALSE, and pattern-variable binding to OP_LOAD/OP_STORE —
     * the same instruction repertoire as user-written if/let. */
    OP_ENTER = 44,               /* nslots(4 bytes) — reserve stack space for locals */
    OP_CCALL_NAME = 45,          /* sym_idx(4 bytes), nargs(1 byte) — name-based CCALL */
    OP_NE = 46,                  /* != — string-aware inequality (mirror of OP_EQ) */
    OP_PUSH_FLOAT = 47,          /* len(4), decimal digits (len) — float literal; the
                                compiler carries the literal as a decimal string
                                (the bootstrap language has no floats) and the VM
                                parses it with strtod at runtime */
    OP_RESERVED_RECV_AFTER = 48, /* was OP_RECV_AFTER — now BUILTIN_RECV_AFTER */
    OP_BUILTIN = 49,             /* legacy three-word form (opcode, idx, n) — cold-path
                                  * actor primitive; now unused, see the NOTE above */

    /* New opcodes must append here (never renumber): the committed
     * lib/bootstrap.tabc seed embeds opcode numbers and must
     * keep running on the new binary. OP_IS_SYMBOL: type test for TAG_SYM,
     * the last of the type-test predicates to get a real opcode (the old
     * bootstrap compiler's detect-by-elimination symbol? helpers predate it). */
    OP_IS_SYMBOL = 50, /* stack: pop v, push (v is TAG_SYM) */

    OP_COUNT
} OpCode;

/* ============================================================
 * Actor primitives — builtin function-pointer table (src/builtin.c)
 *
 * The cold-path actor primitives (spawn / send / receive / monitor …) are
 * not their own opcodes any more: compile.ta emits the three-word
 * `OP_BUILTIN, id, n` instruction, and do_builtin in src/vm.c dispatches
 * on the id (LB_*), delegating to builtin_table[] where a shared
 * implementation is used.  They run on the
 * "cold path" — a builtin owns p->sp for the duration of the call
 * (the do_builtin handler in vm.c publishes sp before it and reloads it
 * after), so the hot-path loop-local stack pointer never has to be threaded
 * into them.  Every entry has the uniform signature
 *
 *     BStatus f(VM *vm, Proc *p)
 *
 * and reports whether the proc finished the instruction (B_OK) or blocked
 * and must re-run the whole builtin instruction once the scheduler wakes
 * it (B_SUSPEND). A blocked builtin returns B_SUSPEND *still holding
 * p->mbox_lock*, leaving p->state untouched: the do_builtin handler in vm.c
 * publishes the resume point (Proc 恢复态字段) and PROC_WAIT_RECV under that lock
 * before unlocking, so a waker can never observe the block state before the
 * resume point is final (see the handler comment in src/vm.c). The instructions that stay opcodes —
 * cons / car / cdr / the type tests / arithmetic / divzero — are cheaper than one indirect call,
 * which is the admission rule for this table.
 * ============================================================ */
typedef enum { B_OK, B_SUSPEND } BStatus;

typedef BStatus (*BuiltinFn)(VM *vm, Proc *p);

/* Internal indices of builtin_table[] in src/builtin.c.  旧 ISA 的
 * spawn/send/recv 条目已随 lispvm 替换删除（do_builtin 的 LB_* switch 是
 * 唯一 id 空间）；剩下的四个是 vm.c do_builtin 借道复用实现的共享处理器，
 * 这里的序号只是表内布局，不是编译 id 空间。 */
typedef enum {
    BUILTIN_RECV_PEEK,   /* selective receive: peek next mbox msg or block */
    BUILTIN_RECV_COMMIT, /* selective receive: consume matched msg, reset scan */
    BUILTIN_RECV_AFTER,  /* wait for next msg up to a deadline; nil on timeout */
    BUILTIN_MONITOR,
    BUILTIN_COUNT
} BuiltinId;

extern const BuiltinFn builtin_table[BUILTIN_COUNT];

/* ============================================================
 * C API — lifecycle
 * ============================================================ */

VM *vm_new(void);
void vm_free(VM *vm);

/* loading */
void vm_register(VM *vm, const char *name, Val (*fn)(VM *vm, Val *args, int nargs), int nargs);
void vm_register_module(VM *vm, const char *name, TaFunc *funcs, int nfuncs);
/* Freeze the registry's host prefix: everything registered so far is a
 * statically linked C module of host_init. Call once, when host_init is
 * done, before any file is loaded. */
void vm_seal_host_modules(VM *vm);
int vm_find_cfunc(VM *vm, const char *name);
/* Monotonic clock in milliseconds (CLOCK_MONOTONIC). Shared by src/net.c
 * (connect deadlines) and src/scheduler.c (I/O poller deadline wakes). */
void vm_register_http_module(VM *vm);

/* execution */
void vm_poller_start(VM *vm);
void vm_poller_stop(VM *vm);

/* 解释器本体（src/vm.c）：每次跑一片后返回下面三个码之一，entry RET 置
 * vm->entry_done + exit_val。唯一调用者 = 运行循环（scheduler.c 的
 * worker_loop），它按返回码决定是否重新入队 —— 解释器自身永不入队
 * （MT 硬点 a：入队即发布，发布后不得再触碰 p）。恢复态内联在 Proc 里
 * （见 Proc 的恢复态字段组），随 Proc 生死，无侧表。 */
#define R_DIED 0 /* proc 终止（entry RET = 程序结束；子 proc RET = 退休） */
#define R_BLOCKED 1 /* 已阻塞（recv 登记 WAIT_RECV / WAIT_IO），等投递或 io poller 唤醒 */
#define R_YIELD 2 /* 预算耗尽（= profiler 采样边界）：state 仍 RUNNING，调用者重新入队 */
int run_proc(VM *vm, Proc *p);

/* 运行循环（src/scheduler.c）：起 io poller、把 entry 入 runq、跑
 * worker_loop，收尾停 poller。宿主 main 只调它一次。 */
void vm_run(VM *vm, Proc *entry);

/* 进程级报错出口：打 "tavm: <msg>" 后 exit(1)（定义在 src/vm.c）。 */
_Noreturn void fatal(const char *msg);

/* stack walking & fn-name resolution — shared by the sampling profiler
 * (prof.c) and the crash report (proc_die in scheduler.c). vm_walk_stack
 * fills out[] leaf..root (current fn first) and returns the depth; tavm
 * registers a walk_stack hook for its own frame graph. vm_fn_name returns
 * the name from fn_names, or NULL if unknown. */
int vm_walk_stack(const VM *vm, const Proc *p, int *out, int max_depth);
const char *vm_fn_name(const VM *vm, int fid);

/* yield API — lets C functions (e.g. net I/O) suspend the current proc
 * without polluting the value space or intruding into opcode logic. */
void vm_watch_fd(VM *vm, int fd, short events);
void vm_yield(VM *vm);
void vm_die(VM *vm, const char *reason);
/* Wake the I/O poller so it re-scans deadlines/fds while blocked in poll()
 * (multi-thread mode only; a no-op in single-thread mode). */
void vm_wait_register(VM *vm, Proc *p);
int vm_wait_count(VM *vm);
void vm_wait_unregister(VM *vm, Proc *p);
void vm_wake_poller(VM *vm);
/* procs_lock 下的短锁读（见 scheduler.c proc_lookup）：poller 线程等
 * worker 之外的上下文必须经此读 procs[]，不能裸读指针数组。 */
Proc *vm_proc_lookup(VM *vm, int pid);

/* scheduler API — process lifecycle, mailbox, run queue (scheduler.c) */
void runq_enqueue(VM *vm, int pid);
int runq_trydequeue(VM *vm);
Proc *proc_new(VM *vm);
/* 建帧：物化首帧帧头（fn 槽 + 实参 nil + cont 区 3 nil）、floor 记 1+maxd，
 * 并把首跑恢复点（pc/base/cbase）发布进 Proc 的恢复态字段 —— spawn 与 entry
 * 共用，run_proc 只剩恢复路径。fnval = 子 proc 的闭包（entry 传 nil）。 */
void proc_frame_init(VM *vm, Proc *p, int fnid, Val fnval);
void proc_die(VM *vm, Proc *p, Val reason);
void mbox_deliver(VM *vm, Proc *target, Val msg);
Val mbox_pop(Proc *p);
/* Message fragment serialization (timer.send_after snapshots the message
 * into malloc'd memory so it survives the caller's heap collections). */
int frag_calc_size(Val root);
Val frag_copy(MsgFragment *f, Val root);

/* profiler API (prof.c) — reduction-boundary sampling, --profile flag */
void prof_init(VM *vm, const char *out_path); /* out_path base, NULL = "profile" */
void prof_finish(VM *vm);                     /* dump + free; idempotent */
void prof_collect(VM *vm, Proc *p, uint64_t dt_ns);
uint64_t prof_now_ns(void);

/* debug */
void print_val(VM *vm, Val v);

/* REPL */
Val vm_eval(VM *vm, const char *src);

/* ============================================================
 * C API — value constructors
 * ============================================================ */

/* val_int is static inline in ta_inline.h (the arithmetic opcodes build an
 * int result on every execution — it must not be an out-of-line call). */
Val val_float(double d);
Val val_nil(void);
Val val_true(void);
Val val_false(void);
Val val_symbol(uint32_t idx);
int vm_intern_symbol(VM *vm, const char *name);

/* api.c — free every token vector owned by p (proc_die cleanup; also
 * called from vm_free for procs that never went through proc_die). */
void vm_free_proc_tokvecs(Proc *p);
Val val_pid(uint32_t pid);

/* heap-allocated constructors (require process context) */
Val val_pair(Proc *p, Val car, Val cdr);
Val val_string(Proc *p, const char *data, int len);
Val val_bytes(Proc *p, const uint8_t *data, int len);

/* ============================================================
 * C API — value predicates & accessors
 * ============================================================ */

/* val_get_int / val_get_float / val_to_double / val_from_double are static
 * inline in ta_inline.h, alongside val_is_int / val_is_float (all of them sit
 * on the arithmetic/comparison hot paths — they must not be out-of-line
 * calls; as out-of-line calls each OP_ADD cost 3-4 real `bl`s).
 *
 * Contract of the float conversions — a float value is a normal
 * (non-NaN-boxed) double, i.e. a value whose top 16 bits fall outside the
 * tag range [TAG_FIRST, TAG_LAST]; see ta_inline.h / the ta.h tag table.
 * val_to_double widens int → double for mixed arithmetic; val_from_double
 * never narrows back to int (any op involving a float stays float). */

int val_is_nil(Val v);

/* val_is_true / val_get_car / val_get_cdr are static inline in ta_inline.h —
 * they sit on the OP_JIF/OP_CAR/OP_CDR hot paths of both dispatchers and
 * must not be out-of-line calls. */
int val_is_pair(Val v);

int val_is_symbol(Val v);
uint32_t val_get_symbol(Val v);

int val_is_clos(Val v);
int val_is_pid(Val v);
uint32_t val_get_pid(Val v);

int val_is_pid(Val v);

int val_is_string(Val v);
HeapString *val_get_string(Val v);

int val_is_bytes(Val v);
HeapBytes *val_get_bytes(Val v);

/* Comparison/equality semantics used by the opcode dispatch (src/vm.c
 * LOP_EQ/LOP_LT/...) — helpers all live in src/vm.c since the vm.c/main.c
 * split, except cmp_numeric_path,
 * a static inline in ta_inline.h (fires on every comparison in both dispatch
 * loops; int/float tower, issue #92/#159). One implementation, no fork:
 *   val_equal — strings by content, immediates by value, heap by identity */
int val_equal(Val a, Val b);

/* ============================================================
 * Deep copy
 * ============================================================ */

Val val_deep_copy(Proc *target, Val v);
/* Deep copy for callback convergence: chunk objects rebuild in the heap,
 * arena (heap) objects pass through untouched, immediates as-is. */
Val val_converge_copy(Proc *target, Val root);
/* Upper bound of the heap bytes val_deep_copy(target, v) will allocate. */
int val_calc_heap_size(Val v);

/* ============================================================
 * Utility — dynamic array growth macro
 * ============================================================ */
#define DA_GROW(ptr, count, cap)                                                                   \
    do {                                                                                           \
        if ((count) >= (cap)) {                                                                    \
            (cap) = (cap) ? (cap) * 2 : 16;                                                        \
            (ptr) = realloc((ptr), (cap) * sizeof(*(ptr)));                                        \
        }                                                                                          \
    } while (0)

/* ============================================================
 * Garbage collection
 *
 * Collectors run in place from proc_heap_alloc, gated by Proc.gc_gate: a
 * collection only happens while no code holds a live heap reference outside
 * the TA stack, so no value ever has to be rooted. The arena is a fixed
 * reservation that never moves (a collection swaps its two semispaces).
 * See docs/design-decisions.md D11.
 * ============================================================ */

int gc_collect(Proc *p, int extra_room);
/* Convergence slow path: a collection that also promotes reachable chunk
 * objects (issue #160). Roots are the TA stack plus *extra_root (updated in
 * place). Returns 0; running out of cap is fatal here — a callback result
 * that cannot fit is a genuine out-of-memory. */
void gc_collect_converge(Proc *p, Val *extra_root);

/* All static inline helpers (proc_push/pop/peek, proc_heap_alloc,
 * proc_arena_grow, val_as_pair/clos, etc.) live here: */
#include "ta_inline.h"

#endif /* TA_H */
