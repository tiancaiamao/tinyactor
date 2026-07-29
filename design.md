# TinyActor — 嵌入式 Actor 并发脚本语言

## 1. 定位

一个用 C 实现、可自举的轻量级 Actor 语言 + VM。
核心理念：**给 C 程序加上 actor 级别的并发能力，且编译器自身用 TA 编写。**

类比：
- Lua 嵌入 C → 轻量脚本，无并发
- TinyActor 嵌入 C → 轻量脚本 + actor 并发 + 自举编译器

目标用户：C 程序员，需要一个简单的并发解决方案。
嵌入感：类似 Lua——`vm_new()` / `vm_load()` / `vm_run()`。

## 2. 核心设计决策

| 决策点 | 选择 | 理由 |
|--------|------|------|
| 并发模型 | 纯 Actor（spawn/send/recv） | 归属清晰，GC 隔离完美，channel 可用 actor 模拟 |
| 消息类型 | int, symbol, pair, pid, string, bytes | 覆盖实用场景 |
| recv 语义 | FIFO，selective matching | |
| 进程死亡感知 | monitor（单向通知） | 比 link 简单，supervisor 可在之上构建 |
| GC | Per-process semispace copying | ~80 行 runtime，进程独立 stop-the-world |
| 调度 | 多线程 M:N（work-stealing） | 多核并行，reduction counting 防饥饿 |
| 尾调用 | 必须优化 | actor 主循环是无限递归，不优化会栈溢出 |
| 赋值 | 完全不可变 | send 深拷贝无循环引用问题，GC 不需要 write barrier |
| 值表示 | NaN-boxing（64 位） | 小整数/pid/symbol 立即值，pair/closure/string/bytes 指向堆 |
| 字节码 | 栈机，~40 条指令 | 栈机实现简单，代码紧凑 |
| 语言语法 | ML/Rust 系（.ta） | 比 Lisp 可读性更好，匹配/解构表达力强 |
| 编译器 | 自举（TA 写 TA 编译器） | 终极验证语言完备性 |

## 3. 语言设计

### 3.1 语法摘要

当前用户语言是 `.ta`（ML/Rust 系语法），不是 Lisp。

```ta
// 函数定义
fn add(x, y) { x + y }

// 带类型注解
fn add(x: int, y: int) -> int { x + y }

// 公开函数
pub fn max(a, b) { if a > b { a } else { b } }

// 变量绑定（不可变）
let x = 42
let y = x + 1

// 条件
if x > 0 { print("positive") } else { print("non-positive") }

// 模式匹配
match x {
  Red -> 1
  Green -> 2
  _ -> 3
}

// 模式匹配 + 解构
match msg {
  ['DOWN, ref, pid, reason] -> print("process died")
  Ping(from) -> send(from, Pong)
  _ -> print("unknown")
}

// Actor 原语
let pid = spawn(fn { worker() })
send(pid, 'hello)
let msg = recv()
let self_pid = self()
let ref = monitor(pid)

// 尾递归（TCO 保证）
fn loop(n) { if n == 0 { 'done } else { loop(n - 1) } }

// 匿名函数
fn(x) { x + 1 }
fn { print("hello") }
```

### 3.2 完整示例

#### ping-pong

```ta
fn ping(n, pong_pid) {
  if n == 0 {
    print("ping done")
    send(pong_pid, 'stop)
  } else {
    send(pong_pid, cons('ping, self()))
    match recv() {
      'pong -> ping(n - 1, pong_pid)
    }
  }
}

fn pong() {
  match recv() {
    'stop -> print("pong done")
    cons('ping, sender) -> {
      send(sender, 'pong)
      pong()
    }
  }
}

fn main() {
  let pong_pid = spawn(fn { pong() })
  spawn(fn { ping(10000, pong_pid) })
  recv()  // wait for pong to finish
}
```

#### supervisor 模式

```ta
fn worker(id) {
  let msg = recv()
  if msg == 'crash { 1 / 0 } else { print(msg); worker(id) }
}

fn sup_loop(pid, ref, count) {
  match recv() {
    ['DOWN, r, p, reason] -> {
      print("worker died")
      if count < 2 {
        let new_pid = spawn(fn { worker(count + 1) })
        let new_ref = monitor(new_pid)
        send(new_pid, 'crash)
        sup_loop(new_pid, new_ref, count + 1)
      } else {
        print("giving up")
      }
    }
  }
}

fn main() {
  let pid = spawn(fn { worker(0) })
  let ref = monitor(pid)
  send(pid, 'crash)
  sup_loop(pid, ref, 0)
}
```

#### HTTP 服务器

```ta
import net
import http

fn handle_request(conn, parsed) {
  let method = car(parsed)
  let path = cdr(parsed)
  if str.eq(path, "/") {
    respond(conn, 200, "text/html", "<h1>Hello from TinyActor!</h1>")
  } else {
    if str.eq(path, "/api") {
      respond(conn, 200, "application/json", "{\"status\":\"ok\"}")
    } else {
      respond(conn, 404, "text/plain", "Not Found")
    }
  }
}

fn respond(conn, status, content_type, body) {
  let resp = http.response(status, content_type, body)
  net.write(conn, resp)
  net.close(conn)
}

fn handle_client(fd) {
  let data = net.read(fd)
  match data {
    'eof -> net.close(fd)
    _ -> {
      let parsed = http.parse_request(data)
      match parsed {
        nil -> net.close(fd)
        _ -> handle_request(fd, parsed)
      }
    }
  }
}

fn accept_loop(server_fd) {
  let client_fd = net.accept(server_fd)
  spawn(fn { handle_client(client_fd) })
  accept_loop(server_fd)
}

fn main() {
  let server_fd = net.listen(8080)
  if server_fd == -1 {
    print("failed to listen on port 8080")
  } else {
    print("HTTP server listening on port 8080")
    accept_loop(server_fd)
  }
}
```

## 4. VM 架构

### 4.1 值的表示（NaN-boxing）

64 位 IEEE 754 double 的 NaN 空间编码所有类型：

```
正常 double   → 原样存储，直接使用
NaN-boxed:
  0xFF00       → int48（小整数）
  0xFF01       → nil
  0xFF04       → symbol（指向 intern 表）
  0xFF05       → pair（指向堆）
  0xFF06       → pid（actor 进程标识）
  0xFF07       → closure（指向堆）
  0xFF08       → string（指向堆）
  0xFF09       → bytes（指向堆）
  0xFF0A       → bool（true/false）
```

### 4.2 字节码指令（约 40 条）

| 类别 | 指令 | 说明 |
|------|------|------|
| 常量 | PUSH_NIL, PUSH_TRUE, PUSH_FALSE, PUSH_INT8, PUSH_INT, PUSH_SYM, PUSH_STRING | 加载立即值 |
| 变量 | LOAD, STORE | 局部变量读写 |
| Pair | CONS, CAR, CDR | 列表构造与解构 |
| 算术 | ADD, SUB, MUL, DIV, MOD | 整数运算 |
| 比较 | EQ, LT, LE | 比较运算 |
| 判断 | IS_NIL, IS_PAIR, IS_INT, IS_STRING, IS_BYTES, IS_PID | 类型谓词 |
| 控制 | JUMP, JUMP_IF_FALSE, POP, DUP | 分支控制 |
| 函数 | CLOSURE, CALL, TAIL_CALL, RET | 函数调用 |
| Actor | SPAWN, SPAWN_MAIN, SPAWN_CLOS, SEND, RECV, RECV_PEEK, RECV_COMMIT, SELF | Actor 原语 |
| 监控 | MONITOR, MONITOR_DEMON | 进程监控 |
| 模块 | LOAD_MODULE, CALL_INDIRECT | 多模块支持 |

### 4.3 进程模型

```
struct Proc {
  Val *stack;             // 栈
  Val *stack_top;
  Val *stack_bot;
  MemHeap heap;           // semispace GC 堆
  Mailbox inbox;          // 消息队列（链表）
  ProcState state;        // RUNNING / WAIT_RECV / WAIT_IO / DEAD
  Proc *next;             // 就绪队列链表
  int pid;
  int reduction_count;    // 时间片计数
  // ...
}
```

- 每个进程独立堆，GC 只扫描自己
- 消息发送时深拷贝（值不可变，无循环引用无需 visited）
- 调度器多线程 work-stealing（就绪队列全局共享）

### 4.4 GC

Per-process semispace copying GC：

```
┌──────────┐    ┌──────────┐
│   from   │    │   to    │
│  space   │ →  │  space  │
│ (active) │    │ (empty) │
└──────────┘    └──────────┘
  已分配满        新分配
                   │
              GC触发后交换角色
```

- 每个进程两个半区，总有一个为空
- GC 时扫描栈 + 闭包 + 邮箱做根集
- 存活对象复制到 to-space，然后交换半区
- 堆满时 realloc 扩容

## 5. 架构一览

```
┌─────────────────────────────────────────────────────┐
│                   用户代码 (.ta)                      │
├─────────────────────────────────────────────────────┤
│  tokenizer.ta → parser.ta → typecheck.ta → codegen.ta│  ← TA 编译器（自举）
├─────────────────────────────────────────────────────┤
│                  bytecode (.tabc)                    │
├─────────────────────────────────────────────────────┤
│  vm.c (解释器 + 调度器 + GC)  ←  C 运行时（~4100 行） │
│  api.c / buf.c / str.c / file.c / net.c / http.c    │
└─────────────────────────────────────────────────────┘
```

## 6. 项目结构

```
├── src/               C 运行时
│   ├── vm.c           字节码解释器 + actor 调度器
│   ├── gc.c           per-process semispace GC
│   ├── val.c          NaN-boxing 值表示
│   ├── api.c          VM 自省 + 模块注册
│   ├── buf.c          字节缓冲区
│   ├── str.c          字符串操作
│   ├── file.c         文件 I/O
│   ├── net.c          TCP 网络
│   ├── http.c         HTTP 解析
│   └── main.c         CLI 入口
├── lib/               TA 编译器（自举）
│   ├── tokenizer.ta   词法分析
│   ├── parser.ta      语法分析 + pattern desugar
│   ├── codegen.ta     字节码生成
│   ├── typecheck.ta   Hindley-Milner 类型推断
│   ├── driver.ta      模块解析 + 管线编排
│   └── bootstrap.tabc 种子编译器（已验证固定点）
├── test/
│   └── scripts/       68 个测试脚本
├── example/
│   └── scripts/       echo 服务器 / HTTP 服务器 / 并发测试
├── ta.h               公共头文件
├── ROADMAP.md         路线图
└── BOOTSTRAP.md       自举架构详细说明
```

**总代码量：~4100 行 C + ~5300 行 TA + ~555 行头文件**

## 7. 验收场景（已全部验证通过）

### 基础语言能力
- fib(30) → 832040 ✅
- 闭包正确捕获变量 ✅
- 尾调用优化：500 万次递归不爆栈 ✅

### Actor 消息传递
- ping-pong 互发消息 ✅
- recv 阻塞/唤醒 ✅
- self/send 循环 ✅

### 抢占式调度
- 死循环进程不卡死其他进程 ✅
- 就绪队列多进程轮流执行 ✅

### 进程隔离
- 进程崩溃不影响其他进程 ✅
- 进程独立 GC ✅

### Monitor
- monitor 收到 DOWN 消息 ✅
- 监控已死亡进程立即收到 DOWN ✅
- supervisor 重启模式 ✅

### 类型系统
- HM 类型推断 ✅
- ADT 构造器 + 模式匹配 ✅
- 穷尽性检查 ✅
- 类型注解验证 ✅

### 模块系统
- import 递归解析 ✅
- pub 可见性控制 ✅
- 模块搜索路径 ✅

### 自举
- 固定点验证通过 ✅
- 201 个测试全部通过 ✅