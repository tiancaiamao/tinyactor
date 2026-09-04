# TinyActor (TA) Language Specification

## 语言概述

**设计定位**：Erlang 风格的 actor 模型 + ML/Rust 系语法的自托管语言。编译器自身用 TA 编写（`lib/parser.ta`、`lib/typecheck.ta`、`lib/codegen.ta`、`lib/tokenizer.ta`），编译到字节码在 C VM 上运行。

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
| 整数 | `42`, `-3`, `0` | **48 位**有符号整数（NaN-boxing int48，见[「整数语义（int48）」](#整数语义int48)） |
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

### use 绑定（do-notation）

`use` 是块级 bind 解糖（参考 Gleam 的 `use`）：把一个 monadic 计算绑定到变量，块内其余语句成为延续（lambda body）。

```ta
use x <- expr
rest...
```

等价于：

```ta
(bind expr (lambda (x) rest...))
```

规则：

- `use` **不是保留字**：仅当后跟 `<-` 时才识别为 use 语法（`use x <- expr` 或 `use <- expr`）；`use(x)` 仍是普通函数调用
- `<-` 由 `<` 与 `-` 两个 token 组成（token 层不保留空白信息），因此 `use < -x` 与 `use <-x` 无法区分，一律按绑定解析。若块首需要写 `use` 变量与 `-x` 比较，请先用 `let` 重命名变量或把比较移出块首
- `use <- expr` 丢弃计算结果（绑定到 `_`）
- use 必须位于块首（`let` 之后亦可），可链式使用；块中间（或任何非块首位置）的 use 是 parse error——解糖依赖"块的其余部分"作为 lambda body，非块首位置没有可包裹的延续
- `bind` / `ret` 是普通函数（如 parser combinator 库），typechecker / codegen 对 use 无感知（纯 parse 层解糖）
- 畸形 use（`<-` 后缺表达式）是 parse error

示例（parser combinator）：

```ta
fn parse_two() {
  use k <- take_char('a')
  use v <- take_char('b')
  ret(cons(k, cons(v, nil)))
}
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
| `+` `-` `*` `/` `%` | 算术 | 二元，int48 运算（溢出静默回绕）。除零/模零导致**当前进程**死亡（reason `'divzero`）——详见[「整数语义（int48）」](#整数语义int48) |
| `==` | 相等 | 比较 |
| `<` `>` `<=` `>=` | 比较 | — |
| `&&` `\|\|` | 逻辑与/或 | 二元，短路求值。解析为 `('and ...)` / `('or ...)`，可用于 guard 或普通表达式 |
| `\|>` | 管道（pipe） | 见下方「管道运算符」。解析期 desugar，最低优先级、左结合 |

**没有逻辑非 `!`**。tokenizer 不识别 `!`，parser 也没有对应规则；需要取反时用 `x == false` 或嵌套 `if` 替代。

### 管道运算符 `|>`

`|>` 把左侧值作为**第一个参数**传给右侧的调用：

```ta
5 |> double()        // => double(5)
x |> f               // => f(x)            // 裸函数名
5 |> add(3)          // => add(5, 3)
1 |> Pair(2)         // => Pair(1, 2)      // 构造函数
5 |> f() |> g()      // => g(f(5))         // 左结合链式
```

规则：

- **解析期 desugar**：`a |> b` 在 parser 里直接重写，typecheck / codegen / VM 零改动，不产生新 AST 节点
- **最低优先级**（比 `||` 还松）：`a + b |> f()` 解析为 `f(a + b)`
- **已知限制**：`a || b |> c`（即 `c(a || b)`）能正确**解析**，但当前 typechecker 对 `or` 表达式的类型推断有 bug（`or` 的结果类型变量与 `bool` 错误统一，见 issue #73），暂无法通过 typecheck。算术/比较表达式不受影响
- **左结合**：`a |> f() |> g()` 解析为 `g(f(a))`
- **右侧限制**：必须是**函数调用**、**裸函数名**或**构造函数调用**。其他形式（字面量、中缀表达式、`{ }` 块、`if`/`let`/`match` 等 special form、lambda）是 parse error：

```ta
5 |> 42             // parse error: 字面量
5 |> 1 + 2          // parse error: 中缀表达式
99 |> { show(7) }   // parse error: 块表达式（不能把值"管道进"块）
5 |> 1.5            // parse error: float 字面量
```

  实现上 parser 用**白名单**（RHS head 只能是 symbol 或 `cons`），新增表达式形式默认拒绝，不会静默误解析
- **与 `||` 的不对称性**：`a || b |> c` 解析为 `c(a || b)`（受上述 typechecker 已知限制，暂不可用），但 `a |> b || c` 是 parse error（`|>` 的右侧不允许 `||` 结果）

---

## 整数语义（int48）

> 本章所有语义断言均经过实际运行验证（探针记录见 `.pge/progress.md`），并作为
> `tools/kernfuzz`（golden 对拍 + morph 变换）的语义基准：golden 模型
> `tools/kernfuzz/golden/golden.py` 的 `w48` 与整型算术规则以本章为准。

### 表示与取值范围

整数是 **48 位二进制补码有符号整数**，取值范围 **[-2^47, 2^47 - 1]**，即
[-140737488355328, 140737488355327]。

运行时表示为 NaN-boxing 64 位值：tag `0xFF00`，低 48 位载荷（`val_payload48`），
读取时从第 47 位符号扩展到 64 位（`src/val.c` 的 `val_int` / `val_get_int`）。
任何时刻的 int 值都被归一化到这个区间——把 ≥ 2^47 的值装箱后再读回会变成负数
（`str.to_int("140737488355328")` 实测返回 `-140737488355328`）。

### 回绕语义（静默模 2^48）

`+` `-` `*` 的结果**溢出时静默按模 2^48 回绕**（二进制补码回绕），不报错、不饱和、
不升级为浮点。VM 实现（`src/vm.c` 的 `OP_ADD`/`OP_SUB`/`OP_MUL`）在 int64 中间结果上
运算，装箱（`val_int`）时截断到低 48 位——效果等价于对每个运算结果做
`w48(x) = ((x + 2^47) mod 2^48) - 2^47`。

实测（与 `golden.py` 的 `w48` 测试向量一致）：

```ta
// 2^46 = 70368744177664 由连乘构造后：
2^46 * 2      // => -140737488355328   （+1 溢出即回绕到 -2^47）
2^46 * 4      // => 0                  （2^48 mod 2^48）
2^47 - 1      // => 140737488355327    （-2^47 - 1 回绕到 2^47 - 1）
2^46 * 2^46   // => 0                  （2^92 mod 2^48，见下方 UB 说明）
```

`tools/kernfuzz/golden/golden.py` 的 Python 模型对每次算术运算后同样调用 `w48` 归一化，
与 VM 侧行为**两侧一致**（对拍基准）。

**浮点混合例外**：只要任一操作数是浮点（float 字面量或浮点运算结果），该运算在
double 精度下进行，结果为浮点，**不发生** int48 回绕（`INT48_MIN + 1.5` 得到
`-1.40737e+14` 而非回绕整数值）。

### 除法与取模

`/` 与 `%` 遵循 C 语义：**商向零截断**，**余数符号跟随被除数**（与 Python 的 floor
除法不同）：

```ta
-7 / 2    // => -3
-7 % 2    // => -1   （不是 Python 的 1）
7 % -2    // => 1
```

`INT48_MIN / -1` 不设陷阱：2^47 装箱回绕，结果仍是 `INT48_MIN`
（实测 `-140737488355328`）；`INT48_MIN % -1` 为 `0`。

### 除零 / 模零：进程死亡协议

整数 `x / 0` 或 `x % 0` 导致**当前进程**死亡（reason `'divzero`），遵循进程隔离协议：

- stderr 打印崩溃报告（`** CRASH pid N: 'divzero`，含函数位置）
- monitor 该进程的一方收到 `['DOWN, ref, pid, 'divzero]`（实测）
- 若死亡的是 main 进程，整个程序以 **exit code 1** 退出
- 若死亡的是 spawn 出的子进程，**其余进程继续运行**，程序 exit code 不受影响
  （实测：子进程 `10 / 0` 崩溃后，父进程继续执行并正常退出）

浮点除零不死亡（IEEE 语义，`1.0 / 0.0` 得到 inf）。

### C 有符号溢出（UB）说明

VM 侧算术由 C 代码执行（int64 中间结果），而 **C 标准中有符号整数溢出是未定义行为**：

- `+` / `-`：两个 int48 操作数的和/差必落在 int64 范围内，**不存在 C 层溢出**，
  模 2^48 回绕是精确定义的实现行为，任何平台/编译器一致。
- `*`：两个 |操作数| 接近 2^47 的乘积可达 2^94，**可溢出 int64**，属于 C UB。
  当前 Makefile 的各构建配置（默认 `-O2` / ASAN / TSAN / COV）**均未加 `-fwrapv`**，
  即回绕行为**没有标准保证**，依赖实际代码生成行为——本机实测（arm64 Apple clang
  `-O2`）乘法按二进制补码回绕（`2^46 * 2^46` → `0`，符合 w48），kernfuzz 对拍据此成立。
  若未来在某个编译器/优化组合上出现偏差，正确做法是给 Makefile 加 `-fwrapv`
  把行为变成 defined，而不是修改 golden 模型。
- `/`：除零已被进程死亡协议拦截，`INT48_MIN / -1` 的结果 2^47 在 int64 内，无 UB。

### 字面量的已知偏差（实现缺口，待裁定）

**运行时算术**严格遵循上述回绕语义；但**字面量路径**目前有偏差：超出 int48 的
十进制字面量在 tokenizer 阶段（`str.to_int` + 装箱）即被 w48 归一化（这一步正确），
随后 codegen 的 `emit_i64`（`lib/codegen.ta`）负数分支只对 [-2^32, -1] 正确
（高 32 位硬编码 `0xFF`），导致 w48 归一化后为负且超出该区间的字面量被写成错误的
字节串。实测：`print(140737488355328)` 输出 `-4294967296`，而按 w48 语义应为
`-140737488355328`。该缺口不改 golden 基准语义，已记录 `.pge/progress.md` 待裁定；
运行中计算出的值不受影响。

---

## 模式匹配

### match 表达式

```ta
match scrutinee {
  pattern -> expr
  pattern when guard_expr -> expr   // guard: arm matches only if guard is true
  pattern -> expr
  _ -> default
}
```

Guard 表达式可以引用 pattern 中绑定的变量（`n when n > 0 -> ...`），
支持 `&&` / `||` 组合，求值为 `false` 时跳过该 arm、尝试下一个。

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

**列表字面量的类型规则**：`[a, b, c]` 是语法糖，解析为 `(list a b c)` 特殊形式
（不是 cons 链），运行期由 codegen 展开回 `cons(a, cons(b, cons(c, nil)))`。
typecheck 要求列表字面量的**所有元素类型互相统一**，异构列表编译报错：

```ta
[1, 2, 3]        // OK
["a", "b"]       // OK
[]               // OK（空表，无元素约束）
[1, "a"]         // type error: cannot unify int with string
["kernfuzz", true]  // type error: cannot unify string with bool
```

手写 `cons(h, t)` 保持异构 pair 语义（`a -> b -> b`），不受此规则约束——
`cons` 是原始 pair 构造器，标准库大量依赖 `('tag . payload)` 结构。

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
- **没有逻辑非 `!`**（`&&` / `||` 支持且短路求值；取反用 `x == false` 或嵌套 `if`）
- **顶层只能定义函数、类型、import**（不能有顶层表达式）
- **不支持箭头类型注解**（`(int -> int)` 等高阶函数注解）
- **类型检查为宽容模式**（类型错误不阻止编译，通过 `--check` 标志报告错误及函数名位置）