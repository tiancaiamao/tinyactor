# TinyActor (TA) Language Specification

## 语言概述

**设计定位**：Erlang 风格的 actor 模型 + Lisp 语法糖的自托管语言。编译器自身用 TA 编写（`lib/parser.ta`、`lib/typecheck.ta`、`lib/codegen.lisp`），编译到字节码在 C VM 上运行。

**编译流程**：
```
source → tokenizer.tokenize → parser.parse → typecheck.infer_program → codegen.compile → VM
```

**运行时**：基于字节码的抢占式调度 VM，多线程 worker，每个 actor 是一个轻量进程，通过消息传递通信。

**类型系统**：Hindley-Milner 类型推导，支持函数注解（`fn f(x: int) -> int`）、复合类型注解（`List(int)`、`Result(int, string)`）和泛型 ADT 声明（`type Color { Red; Green; Blue }`）。类型检查器 (`lib/typecheck.ta`) 已接入编译流程，当前为宽容模式（类型错误不阻塞编译，通过 `--check` 标志报告）。

---

## 值类型

| 类型 | 字面量 | 说明 |
|------|--------|------|
| 整数 | `42`, `-3`, `0` | 64 位有符号整数（NaN-boxing，低 48 位存储） |
| 字符串 | `"hello"` | 不可变，heap 分配 |
| 布尔 | `true`, `false` | — |
| 符号 | `'foo` | 用 `quote` 构造，用于 ADT 变体和模式匹配 |
| Nil | `nil` | 空值/空列表 |
| Pair | `cons(1, 2)` | `(car . cdr)`，列表是 nil 结尾的嵌套 pair |
| 闭包 | `fn(x) { x }`, `fn { ... }` | — |
| Pid | `spawn('worker)` | actor 进程标识符 |

**NaN-boxing 设计**：64 位值中，normal double 原样存储（高 16 位不等于 `0xFFxx`），非 double 类型用高 16 位作 tag（`0xFF00`=int, `0xFF01`=nil, `0xFF04`=sym, `0xFF05`=pair, `0xFF06`=pid, `0xFF07`=closure, `0xFF08`=string）。这意味着浮点数有天然的存储位置，但当前未实现。

**没有浮点数、没有数组/向量、没有可变引用。** 所有值不可变，唯一的状态变化是进程的邮箱。

---

## 语法

### 函数定义

```ta
// 命名函数
fn add(x, y) {
  x + y
}

// 带类型注解（可选，注解存在时 parser 额外发出 type-sig 表单）
fn add(x: int, y: int) -> int {
  x + y
}

// 公开函数（可被其他模块调用）
pub fn max(a, b) {
  if a > b { a } else { b }
}

// 匿名函数（带参数）
fn(x) { x + 1 }

// 匿名函数（无参数，直接执行体）
fn { print("hello") }

// 按名称引用已有函数（传递函数值）
spawn('worker)    // 等价于 spawn(fn { worker() })
```

### 变量绑定

```ta
let x = 42          // 绑定
let y = x + 1       // 后续绑定可引用前面的

// let 在函数体中是顺序的，不支持嵌套 let 作用域语法
// 顶层只能定义函数和类型，不能有顶层 let
```

### 控制流

```ta
// if/else
if x > 0 {
  print("positive")
} else {
  print("non-positive")
}

// if 没有 else 时，else 分支为 nil
if done { print("done") }
```

### 块表达式

```ta
// { } 创建顺序执行块，返回最后一个表达式的值
let result = {
  let a = compute_a()
  let b = compute_b()
  a + b
}
```

### 运算符

| 运算符 | 说明 | 备注 |
|--------|------|------|
| `+` `-` `*` `/` `%` | 算术 | 二元，整数运算。除零导致进程崩溃 |
| `==` | 相等 | 比较 |
| `<` `>` `<=` `>=` | 比较 | — |

**没有逻辑运算符**（`&&`、`||`、`!`）。tokenizer 不识别这些符号，parser 也没有对应的解析规则。codegen 中保留了 `and`/`or` 的编译逻辑（来自更早的 Lisp 方言版本），但 parser 不会产生 `(and ...)` / `(or ...)` 的 AST，所以这些是死代码。需要逻辑运算时用嵌套 `if` 替代。

---

## 模式匹配

### match 表达式

```ta
match scrutinee {
  pattern -> expr
  pattern -> expr
  _ -> default
}
```

### 模式语法

| 模式 | 匹配 | 示例 |
|------|------|------|
| 整数字面量 | 精确匹配 | `42 -> ...` |
| 符号字面量 | 精确匹配 | `'hello -> ...` |
| `nil` | 匹配 nil | `nil -> ...` |
| `true`/`false` | 匹配布尔 | `true -> ...` |
| `cons(a, b)` | 解构 pair，绑定 a/b | `cons(head, tail) -> ...` |
| `[a, b, c]` | 列表模式（语法糖） | `['DOWN, r, pid, reason] -> ...` |
| 裸符号 | 变量绑定，匹配任何值 | `n -> ...` |
| `_` | 通配符，匹配任何值 | `_ -> ...` |

### match 编译方式

parser 将 `match` desugar 为嵌套 `if` + `=` 比较：

```ta
// 源码
match x {
  Red -> 1
  Green -> 2
  _ -> 3
}

// parser 生成
(let temp x
  (if (= temp 'Red) 1
    (if (= temp 'Green) 2
      3)))
```

列表模式 `['DOWN, r, pid, reason]` desugar 为链式 `cons` 解构。

### 穷尽性检查

编译器对 ADT match 进行穷尽性检查（codegen 层面）。如果 match 缺少某个变体，输出 warning 到 stderr：
```
warning: non-exhaustive match: missing Blue
```

---

## ADT（代数数据类型）

### 声明语法

```ta
// 零参变体
type Color { Red; Green; Blue }

// 带参数变体
type Option { None; Some(value) }

// 多字段变体
type Pair { MkPair(a, b) }

// 公开类型（跨模块可见）
pub type Msg { Ping(Pid); Pong; Stop }
```

### 变体在运行时的表示

| 变体类型 | 运行时表示 | 示例 |
|---------|-----------|------|
| 零参 | 符号值 | `Red` → `'Red` |
| 带参 | 函数（构造器） | `Some(42)` → 函数调用，返回包含字段的 pair 结构 |

### parser 生成的 AST

```ta
type Color { Red; Green; Blue }
→ (type Color (quote Red) (quote Green) (quote Blue))

type Option { None; Some(value) }
→ (type Option (quote None) (Some value))

pub type Msg { Ping(Pid); Pong; Stop }
→ (type Msg (Ping (quote Pid)) (quote Pong) (quote Stop))
```

---

## 类型注解

### 函数参数和返回值注解

```ta
fn add(x: int, y: int) -> int {
  x + y
}
```

### parser 行为

- 注解存在时，parser 额外发出 `(type-sig name (param_types...) ret_type)` 表单
- 无注解的函数不发出 `type-sig`，AST 与之前完全一致
- `type-sig` 被代码生成器跳过（不影响编译）

```ta
// 带注解
fn add(x: int, y: int) -> int { x + y }
→ ((type-sig add (int int) int) (define (add x y) (+ x y)))

// 无注解
fn add(x, y) { x + y }
→ ((define (add x y) (+ x y)))
```

### 支持的注解类型

基本类型：`int`, `string`, `bool`, `pid`, `Pid`, 自定义 ADT 名称（如 `Color`）。

复合类型注解（Phase 1 引入）：支持泛型 ADT 应用，如 `List(int)`、`Result(int, string)`、`Option('a)`。不支持箭头类型作为注解（如 `(int -> int)`）。

---

## Actor 模型

### 进程原语

| 操作 | 语法 | 说明 |
|------|------|------|
| 创建进程 | `spawn('fn_name)` 或 `spawn(fn { ... })` | 返回 Pid |
| 发送消息 | `send(pid, msg)` | 异步，消息深拷贝 |
| 接收消息 | `recv()` | 阻塞，取邮箱下一条消息 |
| 接收（选择性）| `receive { pattern -> body }` | 扫描邮箱，跳过不匹配的 |
| 自身 Pid | `self()` | 返回当前进程 Pid |
| 监控 | `monitor(pid)` | 返回 ref，pid 死亡时收到 `['DOWN, ref, pid, reason]` |

### spawn 语义

```ta
// 方式 1：按名称 spawn 一个无参函数
spawn('worker)

// 方式 2：spawn 一个闭包（可捕获变量）
spawn(fn { server(config) })

// 方式 3：spawn 一个匿名函数
spawn(fn(x) { loop(x) })
```

spawn 的函数在**新进程**中运行，有自己的栈和邮箱。

### 消息传递

```ta
// 发送任何值
send(pid, 42)
send(pid, 'hello)
send(pid, cons('data, payload))
send(pid, ['DOWN, ref, dead_pid, reason])   // 列表语法

// 邮箱是 FIFO，但 selective receive 可以跳过
```

### receive vs recv

```ta
// recv() + match：严格 FIFO，取下一条消息
match recv() {
  'ping -> ...
  'pong -> ...
}
// 如果下一条消息不匹配任何分支 → 进程崩溃

// receive { }：选择性接收，扫描邮箱找匹配的
receive {
  'ping -> ...
}
// 跳过不匹配的消息（保留在邮箱中），直到找到匹配的或阻塞
```

### actor 隔离

- 每个 actor 有独立的栈和邮箱
- actor 崩溃（如除零、模式匹配失败）不会影响其他 actor
- `monitor(pid)` 可以检测进程死亡，收到 `['DOWN, ref, pid, reason]`

### 抢占式调度

- VM 多线程 worker（默认按 CPU 核数）
- 每个进程有 reduction 计数（默认 1000），耗尽后抢占切换
- 递归进程不会饿死其他进程

---

## 模块系统

### import

```ta
import tokenizer       // 导入 lib/tokenizer.ta
import parser          // 导入 lib/parser.ta
import msg             // 导入 lib/msg.ta
```

模块解析路径：`lib/{name}.ta`。

### pub 导出

```ta
// pub fn：其他模块可通过 module.fn() 调用
pub fn tokenize(src) { ... }

// pub type：其他模块可使用该 ADT 的变体
pub type Msg { Ping(Pid); Pong; Stop }
```

### 调用导入的函数

```ta
import math

fn main() {
  print(math.abs(-42))      // module.function() 语法
}
```

---

## 内置函数

### 数据操作

| 函数 | 说明 |
|------|------|
| `cons(a, b)` | 构造 pair |
| `car(p)` | pair 的 car |
| `cdr(p)` | pair 的 cdr |
| `null?(x)` | 是否为 nil |
| `pair?(x)` | 是否为 pair |
| `int?(x)` | 是否为整数 |
| `string?(x)` | 是否为字符串 |
| `symbol?(x)` | 是否为符号 |
| `print(x)` | 打印值 |

### 字符串（str 模块）

| 函数 | 类型签名 | 说明 |
|------|----------|------|
| `str.length(s)` | `string -> int` | 字符串长度 |
| `str.concat(a, b)` | `string -> string -> string` | 拼接 |
| `str.eq(a, b)` | `string -> string -> bool` | 比较 |
| `str.char_at(s, i)` | `string -> int -> int` | 第 i 字符的 ASCII 码（-1 越界） |
| `str.substr(s, start, len)` | `string -> int -> int -> string` | 子串 |
| `str.to_int(s)` | `string -> int` | 解析为整数（失败返回 0） |
| `str.from_int(n)` | `int -> string` | 整数转字符串 |
| `str.index_of(s, sub)` | `string -> string -> int` | 查找子串（-1 未找到） |
| `str.to_sym(s)` | `string -> symbol` | 字符串转符号 |
| `str.sym_to_str(sym)` | `symbol -> string` | 符号转字符串 |

### 列表（via pair）

列表是 `nil` 结尾的嵌套 pair。没有内置 list 类型，用 `cons` + `nil` 构建：

```ta
let lst = cons(1, cons(2, cons(3, nil)))   // [1, 2, 3]
```

---

## 自托管编译器

### 标准库（lib/）

| 文件 | 职责 |
|------|------|
| `lib/tokenizer.ta` | 词法分析 |
| `lib/parser.ta` | 语法分析 → AST |
| `lib/codegen.lisp` | 代码生成（Lisp 语法，编译到字节码） |
| `lib/typecheck.ta` | HM 类型推导 + ADT + 注解检查 |
| `lib/driver.ta` | 编译驱动：tokenize → parse → typecheck → codegen → run |
| `lib/math.ta` | 数学工具函数 |
| `lib/msg.ta` | actor 消息类型定义 |
| `lib/buf.ta` | 缓冲区 |
| `lib/file.ta` | 文件 I/O |
| `lib/str.ta` | 字符串工具 |

### Bootstrap

```bash
make tinyactor                     # 构建 C VM
./tinyactor lib/driver.ta file.ta  # 用 TA 编译器编译并运行
make bootstrap                     # 生成 bootstrap 字节码
```

编译器可以用自己编译自己（bootstrap）。

---

## 完整示例

### 基本 actor 系统

```ta
type Msg { Ping(Pid); Pong; Stop }

fn server() {
  match recv() {
    Ping(from) -> {
      send(from, Pong)
      server()
    }
    Stop -> print("done")
  }
}

fn main() {
  let pid = spawn(fn { server() })
  send(pid, Ping(self()))
  match recv() {
    Pong -> print("got-pong")
  }
  send(pid, Stop)
  print("PASS")
}
```

### Supervisor 模式

```ta
fn worker(id) {
  let msg = recv()
  if msg == 'crash {
    1 / 0                        // 故意崩溃
  } else {
    print(msg)
    worker(id)
  }
}

fn supervisor() {
  let pid = spawn(fn { worker(0) })
  let ref = monitor(pid)
  send(pid, 'crash)
  sup_loop(pid, ref, 0)
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
  spawn(fn { supervisor() })
  recv()
}
```

### 尾递归

```ta
// TCO 保证：500 万次迭代不爆栈
fn sum(r, i) {
  if i == 0 {
    r
  } else {
    sum(r + 1, i - 1)    // 尾调用
  }
}

fn main() {
  print(sum(0, 5000000))
}
```

---

## 类型检查

### `--check` 标志

在编译命令后添加 `--check` 标志可启用类型错误报告：

```bash
NWORKERS=1 ./tinyactor --bootstrap source.ta '' --check
```

类型检查器会推导所有函数类型，验证注解，并报告不匹配错误：

```
typecheck: 2 type error(s) found
  in function 'bad_if':   cannot unify int with bool
  in function 'bad_call': cannot unify string with 'a
```

错误信息包含出错的函数名和类型冲突详情。类型错误不会阻止编译——编译器仍然生成字节码并运行程序。`--check` 仅提供类型安全方面的诊断信息。

### 注解强制执行

当函数声明了类型注解时，类型检查器会：

1. **注册声明类型**：`fn f(x: int) -> int` 注册 `int -> int` 作为期望类型
2. **推导实际类型**：从函数体推导实际类型
3. **统一检查**：如果两者不匹配，报告类型错误

未注解的函数不受影响——它们照常推导但不会与声明类型比较。

### 内建函数类型签名

类型检查器内置了常用函数的类型签名，无需注解即可正确推导：

| 分类 | 函数 | 类型 |
|------|------|------|
| 算术 | `+` `-` `*` `/` | `int -> int -> int` |
| 比较 | `<` `>` `<=` `>=` | `int -> int -> bool` |
| 相等 | `==` | `forall a. a -> a -> bool` |
| 布尔 | `not` | `bool -> bool` |
| 列表 | `car` `cdr` | `forall a. a -> a` |
| 构造 | `cons` | `forall a b. a -> b -> b` |
| 谓词 | `null?` `pair?` `int?` `string?` `symbol?` | `forall a. a -> bool` |
| 字符串 | `str.concat` `str.eq` `str.length` 等 | 见上方字符串函数表 |
| Actor | `spawn` `self` | `forall a. a -> pid` |
| 消息 | `send` | `forall a b. a -> b -> b` |
| 消息 | `recv` | `forall a. a` |

---

## 类型系统当前能力边界

### 能力

| 能力 | 状态 | 示例 |
|------|------|------|
| 整数运算推导 | ✅ | `fn f(x) { x + 1 }` → `int -> int` |
| 多态推导 | ✅ | `fn id(x) { x }` → `'a -> 'a` |
| 高阶函数 | ✅ | `fn app(f, x) { f(x) }` → `('a -> 'b) -> 'a -> 'b` |
| 递归函数 | ✅ | `fn fact(n) { ... }` → `int -> int` |
| ADT 变体 | ✅ | `Red` → `Color` |
| 泛型 ADT | ✅ | `type List { Nil; Cons(a, List(a)) }` → `List(int)` |
| Actor 原语 | ✅ | `spawn` → `'a -> pid` |
| 函数注解验证 | ✅ | `fn add(x: int, y: int) -> int` 匹配推导 |
| 复合类型注解 | ✅ | `fn f(xs: List(int)) -> int` 验证参数类型 |
| 类型错误报告 | ✅ | `--check` 标志输出 `in function 'foo': cannot unify int with string` |
| 内建函数签名 | ✅ | `str.from_int` → `int -> string`，`+` → `int -> int -> int` |

### 不支持

| 缺失 | 说明 |
|------|------|
| List 类型 | 没有 `List(a)` 内置类型，nil/cons 组合只推导为 pair + tvar，无法区分空列表和空值 |
| 箭头类型注解 | 不能写 `(int -> int)` 作为参数或返回值注解 |
| 类型错误为硬错误 | 类型检查仍为宽容模式，类型错误不阻止编译（仅 `--check` 时报告） |
| Symbol 基础类型 | 类型系统没有 `symbol` 基础类型，`str.to_sym`/`str.sym_to_str` 使用宽松的多态类型 |

### nil 的类型问题

`nil` 在类型系统中推导为 fresh tvar（`'b`），既是空值又是空列表。HM 无法区分：
- `fn f() { nil }` → `'a -> 'b`（返回值是什么类型？任意）
- `fn make_list() { cons(1, nil) }` → `'a -> 'b`（应该是 `int list`，但推导不出）

对比 Gleam：没有 `nil`，用 `Result(a, b)` / `Option(a)` 代替，List 有明确的 `List(a)` 类型。

---

## 限制与约束

- **没有宏系统**（不像 Cora/Lisp 有 defmacro）
- **没有可变状态**（没有 set!/ref/mutable）
- **没有浮点数**（NaN-boxing 预留了位置，但未实现）
- **没有数组/向量**（只有 pair 和 nil）
- **没有逻辑运算符**（`&&`、`||`、`!` 不存在，用嵌套 `if` 替代）
- **顶层只能定义函数、类型、import**（不能有顶层表达式）
- **不支持箭头类型注解**（`(int -> int)` 等高阶函数注解）
- **类型检查为宽容模式**（类型错误不阻止编译，通过 `--check` 标志报告错误及函数名位置）