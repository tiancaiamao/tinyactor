# TinyActor — 嵌入式 Actor 并发脚本语言

## 1. 定位

一个用 C 实现的、可嵌入 C 程序的轻量级脚本语言 + VM。
核心理念：**给 C 程序加上 actor 级别的并发能力。**

类比：
- Lua 嵌入 C → 轻量脚本，无并发
- TinyActor 嵌入 C → 轻量脚本 + actor 并发

目标用户：C 程序员，需要一个简单的并发解决方案。
嵌入感：类似 Lua——一个 .h + 一个 .c，`vm_new()` / `vm_load()` / `vm_run()`。

## 2. 核心设计决策

| 决策点 | 选择 | 理由 |
|--------|------|------|
| 并发模型 | 纯 Actor（spawn/send/recv） | 归属清晰，GC 隔离完美，channel 可用 actor 模拟 |
| 消息类型 | int, symbol, pair, pid, string, bytes | 覆盖实用场景 |
| recv 语义 | FIFO，selective matching 后期加 | 最简起步 |
| 进程死亡感知 | monitor（单向通知） | 比 link 简单，supervisor 可在之上构建 |
| 错误处理 | Phase 1: 进程直接死；Phase 2: 加 try/catch | try/catch 在 VM 里 ~70 行，架构预留 catch_stack |
| GC | Per-process semispace copying | ~80 行，后期可加分代 |
| 调度 | 单线程，M:N 就绪队列 + reduction counting | 先用单核，后期可加多线程 |
| 尾调用 | 必须优化 | actor 主循环是无限递归，不优化会栈溢出 |
| 赋值 | 完全不可变 | send 深拷贝无循环引用问题，GC 不需要 write barrier |
| C 函数注册 | 编译时注册（vm_load 之前） | 简单干净，满足嵌入场景 |
| Pattern matching | Phase 1: int/symbol/pair/nil/通配符 | 覆盖 90% 用法，后期纯增量扩展 |
| spawn 参数 | 函数名或闭包 | 深拷贝机制已支持闭包，零额外成本 |
| 值表示 | NaN-boxing（64 位） | 小整数/pid/symbol 立即值，pair/closure/string/bytes 指向堆 |
| 字节码 | 栈机，~35 条指令 | 栈机实现简单，代码紧凑 |
| REPL | 支持 vm_eval | 开发友好 |

## 3. 语言设计

### 3.1 语法

S-expression（Lisp 风格），极简内核。

#### 特殊形式

```lisp
;; 变量绑定（不可变）
(let ((x 1) (y 2)) (+ x y))

;; 函数定义
(define (add x y) (+ x y))

;; 条件
(if (> x 0) "yes" "no")

;; 序列
(begin expr1 expr2 ... exprN)

;; 模式匹配（Phase 1 简化版）
(match expr
  (pattern1 result1)
  (pattern2 result2)
  (_ default))

;; Tail-call 位置的递归，栈不增长
;; 编译器自动识别尾位置，生成 TAIL_CALL

;; try/catch（Phase 2）
(try expr (catch e handler))
```

#### Pattern（Phase 1）

```
42              ;; 匹配整数
'hello          ;; 匹配 symbol
'nil            ;; 匹配 nil
(cons a b)      ;; 拆解 pair，绑定 a 和 b
_               ;; 通配符
```

#### Actor 原语

```lisp
(spawn fn-or-closure)       ;; 创建进程，返回 pid
(spawn 'worker)             ;; 按函数名
(spawn (lambda () ...))     ;; 按闭包

(send pid msg)              ;; 发消息（深拷贝到目标进程堆）
(recv)                      ;; 收消息（阻塞直到邮箱非空）
(self)                      ;; 返回当前进程 pid

(monitor pid)               ;; 监控进程，返回 ref
                            ;; 目标死亡时收到 ('DOWN ref pid reason)
```

#### 内置函数

```lisp
;; 算术
(+ a b) (- a b) (* a b) (/ a b) (% a b)

;; 比较
(= a b) (< a b) (> a b) (<= a b) (>= a b)

;; 类型判断
(null? x) (pair? x) (int? x) (string? x) (bytes? x) (pid? x)

;; pair 操作
(cons a b) (car p) (cdr p)
(list a b c ...)  ;; 语法糖

;; 字符串/bytes
(string-length s) (string-concat a b) (string-slice s start end)
(bytes-length b) (bytes-get b idx)

;; 输出
(print x)
```

### 3.2 完整示例

```lisp
;; ping-pong

(define (ping n pong-pid)
  (if (= n 0)
      (begin (print "ping done") (send pong-pid 'stop))
      (begin
        (send pong-pid (cons 'ping (self)))
        (match (recv)
          ('pong (ping (- n 1) pong-pid))))))

(define (pong)
  (match (recv)
    ('stop (print "pong done"))
    (('ping . sender)
     (send sender 'pong)
     (pong))))

(define (main)
  (let pong-pid (spawn 'pong))
  (let ref (monitor pong-pid))
  (spawn (lambda () (ping 10000 pong-pid)))
  (match (recv)
    (('DOWN r pid reason)
     (print "all done"))))
```

```lisp
;; supervisor 模式

(define (supervised-worker)
  (let msg (recv))
  (if (= msg 'crash)
      (/ 1 0)    ;; 故意崩溃
      (begin (print msg) (supervised-worker))))

(define (supervisor)
  (let pid (spawn 'supervised-worker))
  (let ref (monitor pid))
  (sup-loop pid ref))

(define (sup-loop pid ref)
  (match (recv)
    (('DOWN r p reason)
     (when (= r ref))
     (print "worker died, restarting")
     (let new-pid (spawn 'supervised-worker))
     (let new-ref (monitor new-pid))
     (sup-loop new-pid new-ref))))
```

## 4. VM 架构

### 4.1 值的表示（NaN-boxing）

64 位 IEEE 754 double 的 NaN 空间编码所有类型：

```
正常 double   → 原样存储，直接使用
NaN-boxed:
  TAG_INT     (0xFF00) → 低 48 位存 int48（±140 万亿，足够）
  TAG_NIL     (0xFF01) → 固定值
  TAG_TRUE    (0xFF02) → 固定值
  TAG_FALSE   (0xFF03) → 固定值
  TAG_SYM     (0xFF04) → 低 32 位存符号表索引
  TAG_PAIR    (0xFF05) → 低 48 位存进程堆指针
  TAG_PID     (0xFF06) → 低 32 位存 pid
  TAG_CLOS    (0xFF07) → 低 48 位存进程堆指针
  TAG_STRING  (0xFF08) → 低 48 位存进程堆指针
  TAG_BYTES   (0xFF09) → 低 48 位存进程堆指针
```

堆对象（pair、closure、string、bytes）只在本进程堆上有效。
跨进程传递时必须深拷贝。

### 4.2 堆对象结构

```c
// 所有堆对象共享头部
typedef struct {
    uint8_t type;    // HEAP_PAIR | HEAP_CLOS | HEAP_STRING | HEAP_BYTES
    uint8_t flags;   // GC 标记等
} HeapHeader;

typedef struct {
    HeapHeader hdr;
    Val car, cdr;
} HeapPair;

typedef struct {
    HeapHeader hdr;
    int entry;       // 字节码偏移
    int nfree;
    Val free[0];     // 变长：捕获的值
} HeapClosure;

typedef struct {
    HeapHeader hdr;
    int len;
    char data[0];    // 变长
} HeapString;

typedef struct {
    HeapHeader hdr;
    int len;
    uint8_t data[0]; // 变长
} HeapBytes;
```

### 4.3 进程

```c
typedef enum { PROC_RUNNING, PROC_WAIT_RECV, PROC_DEAD } ProcState;

typedef struct {
    int catch_pc;
    int sp;
    int fp;
} CatchFrame;

typedef struct Proc {
    int       pid;
    ProcState state;

    // 执行上下文
    int       pc;
    int       sp;           // 栈顶（偏移量，从 mem 末尾往低地址增长）
    int       fp;           // 栈帧指针
    int       reductions;

    uint8_t  *code;         // 共享字节码（只读引用）
    int      *fn_table;     // 共享函数表（只读引用）
    int       fn_count;

    // 栈 + 堆：一块内存，相向增长
    // 低地址 → [heap ↑] ... [stack ↓] ← 高地址
    uint8_t  *mem;
    int       mem_size;     // 当前分配大小
    int       heap_ptr;     // 堆顶偏移（从低地址往上长）

    // 邮箱（独立分配，按需增长）
    Val      *mbox;
    int       mbox_head, mbox_tail, mbox_count, mbox_cap;

    // Monitor（被谁监控）
    int      *watchers;     // watcher pid 列表
    Val      *watcher_refs; // 对应的 monitor ref
    int       watcher_count, watcher_cap;

    // 错误处理（Phase 2 启用）
    CatchFrame catch_stack[8];
    int        catch_sp;

    // GC（semispace）
    uint8_t  *gc_to;        // to-space
    int       gc_to_size;
} Proc;
```

**初始大小：**
- mem: 256 字节
- mbox: 4 × sizeof(Val)
- watchers: 4 × sizeof(int)
- 结构体本身 ~150 字节
- **总计 ~450 字节/进程**

### 4.4 调度器

```c
typedef struct {
    Proc   **procs;
    int      procs_count, procs_cap;

    int     *runq;          // 就绪队列（pid 数组）
    int      rq_head, rq_tail, rq_count, rq_cap;

    int      next_pid;
    int      next_ref;      // monitor ref 计数器

    uint8_t *code;          // 共享字节码
    int     *fn_table;
    int      fn_count;

    // 符号表（共享，只读）
    char   **symbols;
    int      sym_count, sym_cap;

    // C 函数注册表
    struct {
        char *name;
        Val  (*fn)(VM *vm, Val *args, int nargs);
        int  nargs;
    } cfuncs[MAX_CFUNCS];
    int cfunc_count;
} VM;
```

调度循环：

```
while (runq 非空) {
    pid = dequeue();
    proc = procs[pid];
    if (proc->state != RUNNING) continue;

    for (r = 0; r < MAX_REDUCTIONS; r++) {
        op = code[pc++];
        switch (op) { ... }
        // RECV 且邮箱空 → state = WAIT_RECV, 不重新入队
        // HALT → state = PROC_DEAD, 通知 watchers
    }

    if (state == RUNNING) enqueue(pid);
}
```

### 4.5 字节码指令集

```
栈操作:
  PUSH_NIL
  PUSH_TRUE
  PUSH_FALSE
  PUSH_INT8       i8
  PUSH_INT        i64
  PUSH_SYM        idx

局部变量:
  LOAD            offset     // stack[fp + offset]
  STORE           offset     // stack[fp + offset] = pop()

pair:
  CONS                       // pop cdr, pop car → push pair
  CAR                        // pop pair → push car
  CDR                        // pop pair → push cdr

算术:
  ADD, SUB, MUL, DIV, MOD

比较:
  EQ, LT, LE

类型判断:
  IS_NIL, IS_PAIR, IS_INT, IS_STRING, IS_BYTES, IS_PID

控制流:
  JUMP            addr
  JUMP_IF_FALSE   addr
  POP
  DUP

函数:
  CLOSURE         fn_id, nfree, [offset...]  // 创建闭包
  CALL            nargs                      // 调用函数
  TAIL_CALL       nargs                      // 尾调用（复用栈帧）
  RET

Actor:
  SPAWN           fn_id                     // 按函数名 spawn
  SPAWN_CLOS                                // 栈顶是闭包，spawn 它
  SEND                                      // pop pid, pop val
  RECV                                      // 阻塞等消息
  SELF                                      // push 当前 pid
  MONITOR                                   // pop pid, push ref

字符串/bytes:
  STRING_LEN
  STRING_CONCAT
  STRING_SLICE
  BYTES_LEN
  BYTES_GET

内建:
  PRINT
  HALT
```

~40 条指令。

### 4.6 栈帧布局

```
调用前栈布局：
  [arg0] [arg1] ... [argN-1] [closure]

CALL nargs 执行后：
  ... lower frame ...
  [arg0]        ← fp+N+2  (新 fp 指向这里)
  [arg1]
  ...
  [argN-1]
  [closure]     ← fp+1
  [ret_addr]    ← fp+0     (保存的 pc)
  [old_fp]                   (保存的 fp)
  [free0]                    (闭包捕获的变量)
  [free1]
  ...
  ← sp

TAIL_CALL：不压 ret_addr 和 old_fp，直接覆盖参数区域。
```

### 4.7 消息传递（深拷贝）

send 时遍历值，深拷贝到目标进程的堆：

```
int, symbol, nil, true, false, pid → 立即值，直接复制 8 字节
pair → 递归深拷贝 car 和 cdr，在目标堆上新分配 pair
closure → 深拷贝捕获的值，entry 地址不变（共享字节码）
string → 在目标堆上复制字符串数据
bytes → 在目标堆上复制字节数据
```

不可变数据保证无循环引用，不需要 visited 表。

### 4.8 GC（Semispace Copying）

```
触发时机：heap_alloc 时堆空间不足

过程：
1. 分配 to-space（大小 = from-space）
2. 从栈根（sp 范围内的所有 Val）和邮箱根出发
3. 遍历并复制可达对象到 to-space
4. 交换 from 和 to
5. 更新栈和邮箱中的指针

不可变数据让 GC 更简单：
  - 不需要 write barrier
  - 不需要处理老年代→年轻代引用
  - 复制即可，不需要追踪修改
```

### 4.9 Monitor

```
(monitor pid) 流程：
1. 分配唯一 ref（vm->next_ref++）
2. 在目标进程的 watchers 列表里添加 (当前pid, ref)

进程死亡流程：
1. state = PROC_DEAD
2. 遍历 watchers 列表
3. 对每个 watcher：
   - 构造消息 ('DOWN ref dead_pid reason)
   - 放入 watcher 邮箱
   - 如果 watcher 在 WAIT_RECV，唤醒它（加入就绪队列）
4. 从就绪队列移除自己
```

## 5. C API

```c
// === 生命周期 ===
VM    *vm_new(void);
void   vm_free(VM *vm);

// === 加载 ===
int    vm_register(VM *vm, const char *name,
                   Val (*fn)(VM*, Val*, int), int nargs);
int    vm_load(VM *vm, const char *source);   // 编译并加载，返回 0 成功
int    vm_load_file(VM *vm, const char *path);

// === 执行 ===
int    vm_spawn(VM *vm, const char *fn_name);  // 返回 pid
void   vm_run(VM *vm);                         // 跑到所有进程结束或全部阻塞
int    vm_step(VM *vm, int max_reductions);     // 跑指定步数

// === REPL ===
Val    vm_eval(VM *vm, const char *expr);      // 在新进程里执行表达式

// === C → 脚本值 ===
Val    val_int(int64_t n);
Val    val_nil(void);
Val    val_true(void);
Val    val_false(void);
Val    val_symbol(VM *vm, const char *name);
Val    val_pid(int pid);

// 检查在哪个进程上下文里调，堆分配的值需要进程
Val    val_pair(Proc *p, Val car, Val cdr);
Val    val_string(Proc *p, const char *s, int len);
Val    val_bytes(Proc *p, const uint8_t *data, int len);

// === 脚本值 → C ===
int        val_is_int(Val v);
int64_t    val_get_int(Val v);
int        val_is_nil(Val v);
int        val_is_pair(Val v);
Val        val_get_car(Val v);
Val        val_get_cdr(Val v);
int        val_is_string(Val v);
const char*val_get_string(Val v, int *len);
int        val_is_pid(Val v);
int        val_get_pid(Val v);
```

### C 宿主使用示例

```c
#include "ta.h"
#include <stdio.h>

// 注册一个 C 函数
Val c_sleep(VM *vm, Val *args, int nargs) {
    int ms = val_get_int(args[0]);
    usleep(ms * 1000);
    return val_nil();
}

int main() {
    VM *vm = vm_new();

    // 注册 native 函数
    vm_register(vm, "sleep", c_sleep, 1);

    // 加载脚本
    vm_load(vm,
        "(define (worker id)"
        "  (let msg (recv))"
        "  (print (cons 'got (cons msg (self))))"
        "  (sleep 100)"
        "  (worker id))"
        ""
        "(define (main)"
        "  (let p1 (spawn (lambda () (worker 1))))"
        "  (let p2 (spawn (lambda () (worker 2))))"
        "  (send p1 'hello)"
        "  (send p2 'world))");

    vm_spawn(vm, "main");
    vm_run(vm);

    vm_free(vm);
}
```

## 6. 项目结构

```
tinyactor/
├── Makefile
├── ta.h                    // 公共头文件（嵌入用）
├── src/
│   ├── ta.c                // 合并的单一实现文件（方便嵌入）
│   │
│   ├── val.c               // NaN-boxing 值操作           ~120 行
│   ├── reader.c            // S-expression parser          ~200 行
│   ├── compile.c           // s-exp → 字节码编译器         ~500 行
│   ├── vm.c                // 进程 + 调度器 + 解释器        ~500 行
│   ├── gc.c                // per-process semispace GC     ~100 行
│   ├── cfunc.c             // 内置函数实现                  ~150 行
│   ├── api.c               // C API (vm_new, vm_load 等)   ~200 行
│   └── main.c              // standalone REPL 入口          ~80 行
├── example/
│   ├── ping_pong.c         // C 嵌入示例
│   ├── echo_server.c       // 网络并发示例
│   └── supervisor.c        // supervisor 模式示例
└── test/
    ├── test_vm.c
    ├── test_actor.c
    └── scripts/            // .lisp 测试脚本
        ├── fib.lisp
        ├── ping_pong.lisp
        ├── many_actors.lisp
        └── supervisor.lisp
```

**预估总代码量：~1850 行 C**

## 7. 验收场景

### P0: 基础语言能力
1. `(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2))))) (print (fib 30))` 输出 832040
2. `(let ((x 42)) (print x))` 输出 42
3. 闭包正确捕获变量：`(define (adder n) (lambda (x) (+ x n))) (print ((adder 5) 3))` 输出 8
4. 尾调用优化不会栈溢出：`(define (loop n) (if (= n 0) 'done (loop (- n 1)))) (print (loop 1000000))` 不崩溃

### P0: Actor 消息传递
1. 两个进程 ping-pong 互发 10000 条消息，全部收到
2. `(recv)` 在邮箱空时阻塞进程，有消息后唤醒继续执行
3. `(self)` 返回当前进程 pid，`(send (self) 42) (print (recv))` 输出 42

### P0: 抢占式调度
1. 一个进程跑 `(define (loop) (loop))`，另一个进程正常执行不被卡死
2. 就绪队列中多个进程轮流执行，无饥饿

### P0: 进程隔离
1. 一个进程除零或类型错误崩溃，另一个进程不受影响
2. 一个进程大量分配 pair，GC 只影响自己，不影响其他进程的暂停

### P0: Monitor
1. `(monitor pid)` 返回 ref，目标进程崩溃后收到 `('DOWN ref pid reason)`
2. 监控已死亡的进程，立即收到 DOWN 消息
3. 可以用 monitor 实现 supervisor：worker 崩溃后自动重启

### P0: C 嵌入
1. C 程序 `vm_register` + `vm_load` + `vm_spawn` + `vm_run` 跑通
2. 脚本调用 C 注册的函数，参数正确传递，返回值正确
3. `vm_eval(vm, "(+ 1 2)")` 返回 3

### P1: 深拷贝
1. 发送嵌套 pair `(cons 1 (cons 2 (cons 3 '())))` 到另一个进程，接收方正确拆解
2. 发送闭包到另一个进程，接收方可以调用，捕获的值正确

### P1: GC
1. 进程大量分配后回收，内存使用不无限增长
2. GC 只扫描当前进程的堆，不影响其他进程

### P1: Per-process 内存
1. spawn 10000 个空进程，内存增长线性可控（~450 字节/进程）
2. 进程按需增长：只 recv 等待的进程内存不增长

### P2: try/catch
1. `(try (/ 1 0) (catch e (print e)))` 不崩溃，输出错误信息
2. catch 后进程正常继续运行

### P2: Pattern matching 扩展
1. 嵌套模式：`(match x ((cons (cons a b) c) ...))` 正确拆解
2. Guard 条件：`(match x ((cons a b) (when (> a 0)) ...))`

## 8. 实现阶段

### Phase 1: 能跑（~800 行）
- val.c: NaN-boxing
- reader.c: s-exp parser
- compile.c: 基础编译（define, lambda, if, begin, let, call, tail call）
- vm.c: 栈机解释器 + 进程 + 调度器 + spawn/send/recv/self/monitor
- api.c: vm_new, vm_load, vm_spawn, vm_run, vm_eval, vm_register
- main.c: standalone REPL
- **验收**: fib(30) 正确, ping-pong 跑通, 10000 进程不崩

### Phase 2: 完善（~500 行）
- gc.c: semispace copying GC
- cfunc.c: string/bytes 内置函数
- compile.c: pattern matching, string/bytes 操作
- vm.c: 深拷贝（含闭包）
- **验收**: GC 测试通过, supervisor demo 跑通, C 嵌入示例跑通

### Phase 3: 增强（~300 行）
- vm.c: try/catch (PUSH_CATCH / POP_CATCH)
- compile.c: pattern matching 扩展（嵌套、guard）
- example/: echo server, supervisor 完整示例
- **验收**: try/catch 测试通过, 真实并发 demo 跑通

### Phase 4: 可选优化
- 多线程调度（多核并行）
- 分代 GC
- select/receive 模式匹配
- 网络异步 IO（poller）

## 9. 边界条件

- **邮箱满**：send 到邮箱已满的进程 → 扩容 mailbox（realloc 翻倍）
- **send 到死进程**：消息静默丢弃
- **monitor 死进程**：立即返回 DOWN 消息
- **进程堆耗尽**：触发 GC → GC 后还不够 → realloc 增大 mem → 再不够进程崩溃
- **栈溢出**：栈和堆碰撞 → grow mem
- **无限递归无尾调用**：栈增长直到 mem 耗尽 → GC → grow → 最终进程崩溃
- **vm_eval 在 VM 已有进程运行时**：在新进程里执行，不干扰现有进程
- **深拷贝不可变数据**：无循环引用，不需要 visited 表