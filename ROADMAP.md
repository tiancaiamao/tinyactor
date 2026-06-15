# TinyActor ROADMAP

## 现状

| Phase | 内容 | 状态 |
|-------|------|------|
| 1 | VM 核心：NaN-boxing、reader、compiler、runtime | ✅ |
| 2 | GC（per-process semispace）、string builtins、C FFI | ✅ |
| 3 | 模块系统、net.c、echo/HTTP server | ✅ |
| 4 | 多线程调度器、heap fragment、selective receive、I/O poller | ✅ |

**~4500 行 C，49 个测试，ST + MT 全通过。**

---

## 方向一览（按优先级排序）

| 优先级 | 方向 | Phase | 复杂度 | 依赖 |
|--------|------|-------|--------|------|
| P0 | **新语法（ML/Rust 系）** | 5 | 中 | 无 |
| P1 | **代数数据类型 + 类型标注** | 6 | 高 | P0 |
| P2 | **自举** | 7 | 极高 | P0, P1, P8 ✅ |
| P3 | **模块系统升级** | 8 ✅ | 中 | P0 |
| P4 | **Supervisor / OTP-lite** | 9 | 中 | 无 |
| P5 | **持久化 / Hot Reload** | 10 | 高 | P4 |
| P6 | **分布式** | 11 | 极高 | P8 |

---

## Phase 5: 新语法 — ML/Rust 系（P0）

### 动机

当前 Lisp 语法没有宏，S-expression 的核心优势不存在。嵌套 `if` 可读性差（HTTP server 的 4 层嵌套），`cons` 链构造消息极其笨拙。换语法只影响 `reader.c`，`compile.c` 的 codegen 完全复用。

### 设计理念

借鉴 Gleam / Rust / ML 系语法，但不照搬任何一门语言。
综合语法简单性、实现简单性、表达简洁性，取最合适的设计。

核心 6 关键字：

| 关键字 | 用途 | 说明 |
|--------|------|------|
| `fn` | 定义函数 | `fn name(params) { body }` |
| `let` | 绑定变量 | `let x = expr`，不可变 |
| `match` | 模式匹配 | 结构解构 + 多分支选择 |
| `if` | 布尔条件 | 简单二选一，不替代 match |
| `spawn` | 创建 actor | `spawn(fn { ... })` |
| `send` | 发消息 | `send(pid, msg)` |

推迟到后续 Phase 的特性：
- `|>` 管道操作符 — 推迟到 Phase 8（实现复杂度高，收益有限）
- `pub` 可见性控制 — 推迟到 Phase 8（依赖模块系统）
- `type` 代数数据类型 — Phase 6 专门做

### 具体设计 hint

用 TinyActor 现有代码做直接对照：

#### 1. 函数定义

```
// 现在
(define (handle-client fd)
  (let data (net.read fd))
    ...)

// 新语法
fn handle_client(fd) {
  let data = net.read(fd)
  ...
}
```

要点：
- `fn name(params) { body }` — 大括号分组
- `let x = expr` — 赋值用 `=`，不用括号包裹
- 无 `return`，最后一个表达式是返回值
- `snake_case` 函数名（跟 Gleam/Rust 一致）

#### 2. 模式匹配（取代嵌套 if 链）

```
// 现在 — 4 层嵌套 if
(if (string-eq path "/")
    (respond conn 200 "text/html" "<h1>...</h1>")
    (if (string-eq path "/api")
        ...))

// 新语法 — 扁平 match
match path {
  "/"     -> respond(conn, 200, "text/html", "<h1>...</h1>")
  "/api"  -> respond(conn, 200, "application/json", "{\"status\":\"ok\"}")
  "/time" -> respond(conn, 200, "text/plain", "2025-01-01T00:00:00Z")
  _       -> respond(conn, 404, "text/plain", "Not Found")
}
```

这是**最大的可读性提升**。4 层嵌套 if 变成扁平的 match 分支。

#### 3. Actor 消息 — 类型化（Phase 6 预览）

```
// 现在 — 用 cons 手搓消息结构，极易出错
(send w1 (cons 'msg (cons (self) (cons 'string (cons "hello" 'nil)))))

// 新语法 + 类型（Phase 6）
type WorkerMsg {
  Msg(from: Pid, tag: Symbol, value: a)
  Stop
}

send(w1, Msg(from: self(), tag: 'string, value: "hello"))
```

消息不再是裸 pair 树，而是有名字的结构体。

#### 4. Selective Receive

```
// 现在
(receive
  ('second (print "got-second")))

// 新语法
match receive() {
  'second -> print("got-second")
}

// 带绑定和守卫
match receive() {
  Ping(from)    -> send(from, Pong)
  Stop          -> Done
  n if n > 100  -> print("big")
  _             -> print("other")
}
```

#### 5. 完整 HTTP Server 对比

```
import net
import http

fn handle_request(conn, parsed) {
  let method = car(parsed)
  let path = cdr(parsed)
  match path {
    "/"     -> respond(conn, 200, "text/html", "<h1>Hello from TinyActor!</h1>")
    "/api"  -> respond(conn, 200, "application/json", "{\"status\":\"ok\"}")
    "/time" -> respond(conn, 200, "text/plain", "2025-01-01T00:00:00Z")
    _       -> respond(conn, 404, "text/plain", "Not Found")
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
    eof -> net.close(fd)
    _   -> {
      let parsed = http.parse_request(data)
      match parsed {
        nil -> net.close(fd)
        _   -> handle_request(fd, parsed)
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
  match server_fd {
    -1 -> print("failed to listen on port 8080")
    _  -> {
      print("HTTP server listening on port 8080")
      accept_loop(server_fd)
    }
  }
}
```

对比原始 Lisp 版本，核心改进：
- **嵌套 if → 扁平 match** — 可读性质变
- **`let x = expr`** — 不再需要 `(let x expr)` 后缩进 body
- **`fn { body }`** — lambda 用 `fn { }` 而非 `(lambda () ...)`
- **函数调用用逗号** — `respond(conn, 200, ...)` 而非 `(respond conn 200 ...)`

### 实现策略

- **reader.c 完全重写** — 新 tokenizer + parser，输出相同的 AST（pair 树）
- **compile.c 零改动** — codegen 输入仍然是 pair 树
- **vm.c 零改动** — bytecode 格式不变
- **新增 `.ta` 扩展名**，保留 `.lisp` 向后兼容（双语法共存期）
- **编译器 `reader.c` 只做语法转换**，不碰语义

### 技术要点

- **Tokenizer 需要：**
- `{ }` `()` `,` `;` `->` `=` `:` 等符号
- `fn` `let` `match` `if` `spawn` `send` `receive` `import` 关键字
- 缩进不敏感（用 `{}` 分组），避免 Python/Haskell 的 off-side rule 复杂度

**Parser 需要：**
- `fn name(params) { body }` → 等价于 `(define (name params) body...)`
- `let x = expr` → 等价于 `(let x expr ...)`
- `match subj { pat -> body ... }` → 等价于 `(match subj (pat body...) ...)`
- `receive { pat -> body ... }` → 等价于 `(receive (pat body...) ...)`
- `expr |> f(args)` → 等价于 `(f expr args...)`

**AST 保持 pair 树不变**，新语法只是 pair 树的另一种 concrete syntax。

---

## Phase 6: 类型系统（P1）

### 动机

刚才写 `multithread-basic.lisp` 时，`(cons 1 (cons 2 'nil))` 跟 pattern `('msg from tag value)` 不匹配直接静默失败。类型系统让这类问题编译期暴露。

### 分阶段

#### 6a. 类型标注（Annotation）

```
fn add(a: Int, b: Int) -> Int {
  a + b
}

let x: String = net.read(fd)
```

编译器记录类型信息，但暂不强制检查。这阶段的价值是**文档化** + 为后续推导打基础。

#### 6b. 代数数据类型（ADT）

```
type Msg {
  Ping(from: Pid)
  Pong
  Stop
}

type Result(a, e) {
  Ok(value: a)
  Err(error: e)
}
```

ADT 让 actor 消息有名字、有结构。编译器可以检查 `receive` 是否穷尽了所有 variant。

#### 6c. 类型推导

```
let x = 42          // 推导为 Int
let y = "hello"     // 推导为 String
let z = add(x, 1)   // 推导为 Int（跟 add 的签名一致）
```

不需要写 `: Int`，编译器从上下文推导。仅推导简单类型（Int, String, Pid, Bool）。

#### 6d. 模式匹配穷尽性检查

```
type Color { Red; Green; Blue }

match c {
  Red -> ...
  // 编译错误：Green 和 Blue 未处理！
}
```

这是类型系统最有价值的功能 — 保证 `receive` 和 `match` 覆盖所有情况。

### 实现策略

- `ta.h`：新增 `Type` 结构体（kind + params）
- `compile.c`：新增类型检查 pass，在 codegen 之前运行
- bytecode 格式不变 — 类型信息是编译期的，不进入运行时
- 与 Phase 5 新语法天然配合（`fn f(x: Int)` 比 `(define (f x) ...)` 更自然）

---

## Phase 7: 自举（P2）

### 动机

自举是语言完整度的终极证明。如果 TinyActor 能用自己的语法写自己的编译器，说明语言能力完备。

### 路线图

1. **定义 IR 文本格式** — 当前 bytecode 是二进制的，定义一个人类可读的文本表示
2. **用 TinyActor 写 reader** — 解析 `.ta` 源码 → AST
3. **用 TinyActor 写 codegen** — AST → bytecode（复用 C 版本的 codegen 逻辑）
4. **Bootstrap 测试** — 用 TinyActor 编译器编译自身，对比 C 编译器输出的 bytecode
5. **C 编译器退居 bootstrap 角色** — 只负责第一次编译

### 前置需求

- Phase 5 语法（需要友好的语法来写编译器）
- Phase 6 类型（编译器自身需要类型安全）
- 字节码操作原语（或 C FFI）

### 挑战

当前 TinyActor 缺少：
- 数组/向量（需要字节码操作）
- 字节串操作（需要解析文本）
- 文件 I/O（需要读源文件）

这些需要作为标准库补充，可能通过 C FFI 实现。

---

## Phase 8: 模块系统升级（P3）

### 动机

现在 `(import "net")` 是编译期 no-op，模块必须在 C 侧提前注册。要扩展标准库需要让 TinyActor 能 import 自身写的 `.ta` 文件。

### 目标

```
// math.ta
pub fn abs(n) {
  match n < 0 { True -> -n; _ -> n }
}

// main.ta
import math

fn main() {
  print(math.abs(-42))   // 输出 42
}
```

### 实现策略

- 编译器遇到 `import math` → 递归加载 `math.ta` → 编译为独立 module
- 模块级的 `pub` 可见性控制（Phase 5 关键字）
- 符号表增加模块前缀（`math.abs` → `module_0_fn_3`）
- 保留 C 模块 FFI 作为底层原语

---

## Phase 9: Supervisor / OTP-lite（P4）

### 动机

Erlang 的 "let it crash" 哲学需要 supervisor 保障。现在 actor crash 后只发 DOWN 消息，没有自动重启。

### 设计

```
// one_for_one: 子进程 crash 后重启
supervisor.one_for_one([
  fn { accept_loop(8080) },
  fn { health_check_loop() },
])

// restart策略
type Restart = Permanent | Transient | Temporary
type Shutdown = BrutalKill | Timeout(Int)

pub fn start_child(spec: ChildSpec) -> Pid
```

### 实现策略

- 纯语言层面（不需要改 VM）
- Supervisor 本身就是一个 actor，监听 DOWN 消息 + 重启子进程
- 可以用 Phase 5 语法直接实现，是很好的自举前验证

---

## Phase 10: 持久化 / Hot Reload（P5）

### 动机

Actor 状态可以序列化到磁盘，crash 后恢复。代码热替换不停机。

### 设计

```
// Checkpoint
let state = save_state(my_actor)
file.write("checkpoint.bin", state)

// Restore
let data = file.read("checkpoint.bin")
let my_actor = restore_state(data)

// Hot reload
reload_module("http")  // 不停机替换 http 模块代码
```

---

## Phase 11: 分布式（P6）

### 动因

`spawn(node, fn)` — 跨进程/跨机器创建 actor，消息透明传递。

### 设计

```
// 本地 spawn
let pid = spawn(fn { worker() })

// 远程 spawn
let pid = spawn("node2@example.com", fn { worker() })

// send 不关心 pid 在本地还是远程
send(pid, Msg("hello"))
```

需要网络层（Phase 3 的 net.c 基础）+ pid 编码（本地/远程区分）+ 序列化协议。

---

## 总结：为什么 P0 = 语法？

1. **投入产出比最高** — 只改 reader.c，其余零改动
2. **解锁后续所有 Phase** — 类型标注需要新语法、自举需要友好语法、模块系统需要 `pub/import`
3. **用户体验质变** — 当前 Lisp 语法是最大的开发痛点（写测试脚本时反复踩坑）
4. **风险可控** — AST 不变，只是换了 concrete syntax