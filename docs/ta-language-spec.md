# TinyActor (TA) Language Specification

## 语言概述

**设计定位**：Erlang 风格的 actor 模型 + ML/Rust 系语法的自托管语言。编译器自身用 TA 编写（`lib/bootstrap/parser.ta`、`lib/bootstrap/typecheck.ta`、`lib/bootstrap/compile.ta`、`lib/bootstrap/tokenizer.ta`），编译到字节码在 C VM 上运行。

**编译流程**：
```
source → tokenizer.tokenize → parser.parse → typecheck.infer_program → lower-ast.lower_program → compile.compile → VM
```

**运行时**：基于字节码的抢占式调度 VM，多线程 worker，每个 actor 是一个轻量进程，通过消息传递通信。

**类型系统**：Hindley-Milner 类型推导，支持函数注解（`fn f(x: Int) -> Int`）、复合类型注解（`List(Int)`、`Result(Int, String)`）和泛型 ADT 声明（`type Color { Red; Green; Blue }`）。类型检查器 (`lib/bootstrap/typecheck.ta`) 已接入编译流程，类型错误为硬错误（报告含 `[E00xx]` 错误码，编译中止）。

---

## 值类型

| 类型 | 字面量 | 说明 |
|------|--------|------|
| 整数 | `42`, `-3`, `0` | **48 位**有符号整数（NaN-boxing int48，见[「整数语义（int48）」](#整数语义int48)） |
| 浮点 | `3.14`, `-0.5` | IEEE 754 double（NaN-boxing 原样存储，见[「浮点与严格数值塔」](#浮点与严格数值塔)） |
| 字符串 | `"hello"` | 不可变，heap 分配 |
| 布尔 | `true`, `false` | — |
| 符号 | `'foo` | 用 `quote` 构造，用于 ADT 变体和模式匹配 |
| Nil | `nil` | 空值/空列表 |
| Pair | `cons(1, 2)` | `(car . cdr)`，列表是 nil 结尾的嵌套 Pair |
| 闭包 | `fn(x) { x }`, `fn { ... }` | — |
| Pid | `spawn('worker)` | actor 进程标识符 |
| Resource | —（C 模块 API 产出） | 不透明资源值，见[「资源值的传递限制」](#资源值的传递限制resource) |

**NaN-boxing 设计**：64 位值中，normal double 原样存储（高 16 位落在 tag 区间
`[0xFFF1, 0xFFFC]` 之外），非 double 类型用高 16 位作 tag（`0xFFF1`=int, `0xFFF2`=nil, `0xFFF3`=true, `0xFFF4`=false, `0xFFF5`=sym, `0xFFF6`=pair, `0xFFF7`=pid, `0xFFF8`=closure, `0xFFF9`=string, `0xFFFC`=resource）。tag 区间整体落在 IEEE 754「指数全 1、符号位 1」的负 NaN 区，任何有限 double（指数 ≤ 0x7FE，高 16 位 ≤ 0xFFEF）与 -Inf（恰为 0xFFF0…）都不可能撞上；NaN 位模式在装箱时统一规范化为固定 quiet NaN（issue #169）。浮点数即「原样存储的 normal double」。

**没有数组/向量、没有可变引用。** 所有值不可变，唯一的状态变化是进程的邮箱。

---

## 语法

### 函数定义

```ta
// 命名函数
fn add(x, y) {
  x + y
}

// 带类型注解（可选，注解存在时 parser 额外发出 type-sig 表单）
fn add(x: Int, y: Int) -> Int {
  x + y
}

// 公开函数（可被其他模块调用）
pub fn max(a, b) {
  if a > b { a } else { b }
}

// 匿名函数（带参数）——只能出现在表达式位置（let 值、函数参数等）
let inc = fn(x) { x + 1 };
spawn(fn(x) { loop(x) })

// 匿名函数（无参数，直接执行体）——同样只能出现在表达式位置
spawn(fn { print("hello") })

// 顶层匿名 fn 是 parse error（没有绑定目标，无用途）：
//   fn(x) { x + 1 }        // parse error: anonymous fn not allowed at top level
//   fn { print("hi") }     // 同样被拒绝
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

### use 绑定（do-notation，Gleam 式）

`use` 是块级脱糖（参考 Gleam 的 `use`）：把一个 monadic 计算绑定到变量，块内其余语句成为延续（lambda body）。与 Gleam 一样，**RHS 自己就是延续消费者**——脱糖产物是普通调用节点，语法不承诺任何名为 `bind` 的函数。

```ta
use x <- expr
rest...
```

等价于：

```ta
expr(lambda(x) { rest... })   // 即 (expr (lambda (x) rest...))，与 f(a) 同形的调用节点
```

规则：

- `use` **不是保留字**：仅当后跟 `<-` 时才识别为 use 语法（`use x <- expr` 或 `use <- expr`）；`use(x)` 仍是普通函数调用
- `<-` 由 `<` 与 `-` 两个 token 组成（token 层不保留空白信息），因此 `use < -x` 与 `use <-x` 无法区分，一律按绑定解析。若块首需要写 `use` 变量与 `-x` 比较，请先用 `let` 重命名变量或把比较移出块首
- `use <- expr` 丢弃计算结果（绑定到 `_`）
- use 必须位于块首（`let` 之后亦可），可链式使用；块中间（或任何非块首位置）的 use 是 parse error——解糖依赖"块的其余部分"作为 lambda body，非块首位置没有可包裹的延续
- 脱糖是**纯 parse 层**变换：产物是与 `f(a)` / 链式 `f(x)(y)` 同形的普通调用节点，typechecker / codegen 对 use 无感知。因此 **RHS 必须是吃延续的函数**——`use x <- 42` 是 type error（对 Int 做调用），不是运行时崩溃
- 提供 binder 的库（如 mpc）用包装函数适配此语法：`use v <- mpc.then(p); rest`——`mpc.then(p)` 返回 `fn(k) { bind(p, k) }`，由它消费延续
- 畸形 use（`<-` 后缺表达式）是 parse error

示例（parser combinator，mpc 库）：

```ta
import mpc

fn parse_two(s, i) {
  use k <- mpc.then(take_char('a'))
  use v <- mpc.then(take_char('b'))
  mpc.ret(cons(k, cons(v, nil)))
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
| `+` `-` `*` `/` `%` | 算术 | 二元。`%` 仅 Int；`+ - * /` 严格同型（Int~Int 或 Float~Float，混用是 type error，见[「浮点与严格数值塔」](#浮点与严格数值塔)）。Int 运算溢出静默回绕；除零/模零导致**当前进程**死亡（reason `'divzero`）——详见[「整数语义（int48）」](#整数语义int48) |
| `==` | 相等 | 操作数必须同型（Int/Float 不混用）；与 `nil` 比较豁免。`!=` 同规则 |
| `<` `>` `<=` `>=` | 比较 | 严格同型（Int~Int 或 Float~Float），结果 Bool |
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
- **已知限制**：`a || b |> c`（即 `c(a || b)`）能正确**解析**，但当前 typechecker 对 `or` 表达式的类型推断有 bug（`or` 的结果类型变量与 `Bool` 错误统一，见 issue #73），暂无法通过 typecheck。算术/比较表达式不受影响
- **左结合**：`a |> f() |> g()` 解析为 `g(f(a))`
- **右侧限制**：必须是**函数调用**、**裸函数名**或**构造函数调用**。其他形式（字面量、中缀表达式、`{ }` 块、`if`/`let`/`match` 等 special form、lambda）是 parse error：

```ta
5 |> 42             // parse error: 字面量
5 |> 1 + 2          // parse error: 中缀表达式
99 |> { show(7) }   // parse error: 块表达式（不能把值"管道进"块）
5 |> 1.5            // parse error: Float 字面量
```

  实现上 parser 用**白名单**（RHS head 只能是 Symbol 或 `cons`），新增表达式形式默认拒绝，不会静默误解析
- **与 `||` 的不对称性**：`a || b |> c` 解析为 `c(a || b)`（受上述 typechecker 已知限制，暂不可用），但 `a |> b || c` 是 parse error（`|>` 的右侧不允许 `||` 结果）

---

## 整数语义（int48）

> 本章所有语义断言均经过实际运行验证（探针记录见 `.pge/progress.md`），并作为
> `tools/kernfuzz`（golden 对拍 + morph 变换）的语义基准：golden 模型
> `tools/kernfuzz/golden/golden.py` 的 `w48` 与整型算术规则以本章为准。

### 表示与取值范围

整数是 **48 位二进制补码有符号整数**，取值范围 **[-2^47, 2^47 - 1]**，即
[-140737488355328, 140737488355327]。

运行时表示为 NaN-boxing 64 位值：tag `0xFFF1`，低 48 位载荷（`val_payload48`），
读取时从第 47 位符号扩展到 64 位（`src/val.c` 的 `val_int` / `val_get_int`）。
任何时刻的 Int 值都被归一化到这个区间——把 ≥ 2^47 的值装箱后再读回会变成负数
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

**历史备注（issue #92 已推翻）**：在严格数值塔落地前，本语言曾允许任一操作数为浮点时整个运算切换到 double 语义（「浮点混合例外」，如 `INT48_MIN + 1.5` 得到 `-1.40737e+14`）。该例外让 int48 回绕语义被一个 Float 字面量静默绕过，已于 issue #92 移除：混用现在是编译期 type error，见下一章。运行时 `OP_ADD` 等指令对动态值的数值提升行为保持不变（见「浮点与严格数值塔」末段）。

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

### 字面量路径

**运行时算术**与**字面量路径**遵循同一套 w48 回绕语义。整数字面量在进入值层时
即完成 48 位归一化：tokenizer 对数字串调 `str.to_int`（`lib/str.c`），它返回前
经 `val_int`（`ta_inline.h`）装箱取低 48 位——与运行时算术同一套取模语义。
编译期只把归一化后的整数原样内嵌进指令词（`IConstI` → `OP_CONST_I`），不做
任何字节串拼接。
实测 `print(140737488355328)`（2^47）输出 `-140737488355328`（-2^47），
且 `140737488355328 == -140737488355328` 为 true——字面量与运行时算术对同一
w48 语义，一致。

---

## 浮点与严格数值塔（issue #92）

TA 采用 OCaml/Gleam 式**严格分离**的数值塔：Int 和 Float 是完全不同的类型，
任何运算符都不做隐式转换。

### Float 表示

Float 是 IEEE 754 double，NaN-boxing 下「normal double 原样存储」（高 16 位落在
tag 区间之外，见[「值类型」](#值类型)）。特殊值全部按 IEEE 语义工作（issue #169）：
`1.0 / 0.0` 得到 inf，`0.0 - inf` 得到 -inf（可打印、可比较、可参与算术），`0.0 / 0.0`
得到 nan（NaN 载荷位不保留，打印为 `nan`；`nan != nan` 成立、`nan == nan` 不成立）。
字面量形如 `3.14` / `-0.5`（含小数点或指数即
Float；`3` 是 Int，`3.0` 也是 Float）。源码层面 Float 字面量的文本端到端传递，
VM 侧用 `strtod` 解析（`('float "1.5")` AST 节点）。

### 严格同型规则（编译期）

`+` `-` `*` `/` `<` `<=` `>` `>=` 的所有操作数必须统一到**同一**数值类型：

```ta
1 + 2         // ok：Int~Int
1.5 + 2.0     // ok：Float~Float
3 + 0.5       // type error：cannot unify
2.5 < 3       // type error：cannot unify
```

`%` 仅接受 Int（Float 取模不在范围内）。类型参数（tvar）不受约束地跟随第一个
操作数的类型绑定，泛型代码（如 `fn add(a, b) { a + b }`）仍然可用。

`==` / `!=` 同样要求操作数同型：Int == Float、Bool == Int、String == Int 等
跨类型比较一律 type error（issue #65 的收紧 + issue #92 推翻 `3 == 3.0`
数值扩宽放行）。**唯一豁免：与 `nil` 的比较**——`nil == nil`、`x == nil`、
`nil != false` 等永远合法（nil 是单例，比较不约束另一侧的类型）。

### 跨塔转换函数

| 函数 | 类型 | 语义 |
|------|------|------|
| `int.to_float(n)` | Int -> Float | 精确（int48 载荷升宽为 double） |
| `float.to_int(d)` | Float -> Int | **向零截断**（与 `/` 的商一致；`float.to_int(3.9)` → 3，`float.to_int(-3.9)` → -3） |

`float.to_int` 的越界行为：先向零截断，再按 int48 二进制补码回绕（模 2^48）——
与所有 Int 值装箱时的归一化一致（`src/num.c`）；NaN 截断为 0。

```ta
int.to_float(3) + 0.5        // => 3.5
float.to_int(7.0) / 2        // => 3
```

### 运行时与静态的关系

严格同型只是**静态类型层**的规则；运行时 `OP_ADD` / `OP_EQ` / `OP_LT` 等指令对
动态构造的值仍按原有数值提升语义执行（Int 载荷与 boxed double 混算时升宽为
double）。这一保留是为了不动 VM 语义与 kernfuzz 对拍基准；混用程序在编译期就
被拦截，正常代码不会触达提升路径。

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
| 符号字面量 | 精确匹配 | `'hello -> ...`、`'+ -> ...`（标点符号同样支持） |
| `nil` | 匹配 nil | `nil -> ...` |
| `true`/`false` | 匹配布尔 | `true -> ...` |
| `cons(a, b)` | 解构 Pair，绑定 a/b | `cons(head, tail) -> ...` |
| `[a, b, c]` | 列表模式（语法糖） | `['DOWN, r, pid, reason] -> ...` |
| `[a \| b]` | 列表 rest 模式：b 接在链尾，吸收剩余 spine | `[h \| t] -> ...` |
| `Name(a, b)` | 构造子模式：首元素 `quote` 标签严格相等 + 逐参解构 + 尾 nil 钉死参数个数 | `Ok(v) -> ...` |
| 裸符号 | 变量绑定，匹配任何值 | `n -> ...` |
| `_` | 通配符，匹配任何值 | `_ -> ...` |
| `Name`（大写、无参） | 空参构造子模式（等价 `Name()`） | `Red -> ...` |

其余 token（裸算符 `+`、`<`、`{`、`,` …）都不构成模式，是 parse error——不会被静默
降成 `_`。（issue #373：标点符号字面量过去被静默当成 `'_`，永不命中。）

构造子模式在 parser 层脱糖为**列表模式** `(cons (quote Name) p1 ... nil)`——
与方括号模式 `[Name, p1, ...]` 同一编码：首元素 quote 标签发严格相等判定，
链尾 nil 钉死 arity。构造子的值本身就是这种带标签的列表，所以「模式 = 值
形态」。这是唯一的发射点——宿主 codegen 走既有的 `'cons` 分支，lisp 管线由
`lower_pat` 的链摊平喂给 `compile.ta`，两端不各自维护构造子判别逻辑。

**方括号 / `cons` 链的分工**：同一个 cons 链编码承担两种语义，靠**链尾**区分——

| 拼写 | 编码 | 语义 |
|------|------|------|
| `[a, b, c]` | `(cons a (cons b (cons c nil)))` | **shape 断言**：长度钉死为 3，长度不符该臂不匹配（`match` 无命中臂时静默返回 nil） |
| `cons(a, b)` / `[a \| b]` | `(cons a b)` | **spine 遍历**：a 绑 head、b 绑 tail，不限制长度 |
| `[a, b \| rest]` | `(cons a (cons b rest))` | 前 K 个元素定长 + 活尾巴：匹配长度 >= K，`rest` 绑剩余 spine（类型 `List(E)`） |

`[e1, .. eK \| rest]` 里的 `rest` 顶掉的正是链尾那个 arity-pin 的 nil，所以它不是
新的匹配机制，就是 `cons(e1, .. cons(eK, rest))` 的方括号写法（Erlang `[H \| T]`
血统；Haskell/OCaml 的对应拼写是中缀 `:` / `::`，Gleam 写 `[e1, .. rest]`）。
`|` 只出现在模式里：表达式位置的裸 `|` 是 parse error（`a | b` 不会被当成按位或，
也不会被吞成调用参数）。

方括号定长模式写成 `[Bind(k, v), _]` **不能**吸收 tail——3 元素链表会静默走空。
typecheck 的穷尽性检查把定长臂视为「不覆盖任何 List 形态」（见下），正好把这个坑
变成编译错误；要遍历就改用 `[h | t]` 或 `cons(h, t)`。

### match 编译方式

lower 期（`lib/bootstrap/lower-ast.ta` 的 `match_desugar`）将 `match` 脱糖为嵌套
`if` + `=` 比较（脱糖发生在 `.kl` 序列化**之前**，`.kl` 词表里没有 match；实测
形态见 `docs/kl-format.md` §2.2）：

```ta
// 源码
match x {
  Red -> 1
  Green -> 2
  _ -> 3
}

// lower 生成
(let temp x
  (if (= temp 'Red) 1
    (if (= temp 'Green) 2
      3)))
```

列表模式 `['DOWN, r, pid, reason]` desugar 为链式 `cons` 解构。

### 穷尽性检查

编译器对 ADT match 与 `List(E)` match 做穷尽性检查（typecheck 层）。如果 match
缺少变体且没有 wildcard/binding 臂兜底，编译报错拒绝（Gleam 语义；有意部分匹配
需显式加 `_ ->` 兜底）：
```
[E0005] non-exhaustive match: missing Blue
```

`List(E)` 的两个形态是 `nil`（`nil ->` / `[] ->`）和 `cons`（`cons(h, t)` 或
`[h | t]` 臂，tail 必须是活的变量/通配符而不是 arity-pin 的 nil）。定长方括号臂
（`[a]` / `[Bind(k, v), _]`）钉死长度，不覆盖任何形态：

```ta
fn sum5(xs: List(Int)) -> Int {
  match xs {
    cons(_, _) -> 1
  }
}
// [E0005] non-exhaustive match: missing nil

fn find(key: String, binds: List(Bind)) -> Int {
  match binds {
    [Bind(k, v), _] when k == key -> v
    [_, remain] -> find(key, remain)
    [] -> -1
  }
}
// [E0005] non-exhaustive match: missing cons
```

`receive` 不检查：一个 mailbox 里混有多种消息类型，没有单一 scrutinee 类型可供
约束。

#### 嵌套模式（issue #396）

穷尽性检查是**充分**条件，不是完整判定。一个变体只在某个臂**整体**匹配它时才算
被覆盖：臂在该变体的参数位置上是变量/通配符，或者参数本身是「唯一形态」构造子
（单变体 ADT 如 `Buf(h)`、内置 `Pair(a, b)`）且其参数同样整体覆盖。参数位置上
出现子模式（多变体 ADT 的构造子、字面量、列表形态）时，该变体只算被**部分**覆盖：

```ta
type Opt { Some(a) None }

fn bad(o : Opt) -> Int {
  match o {
    Some(Some(x)) -> 1     // 只挑了 Some 的一个子情形
    None -> 0
  }
}
// [E0005] cannot prove exhaustive match: nested patterns cover only part of Some
//          — add an explicit `_ ->` arm
```

旧检查只看顶层构造子集合，`Some(None)` 会掉到运行期；这条把它变成编译错误。

代价是**真正穷尽、但需要交叉推理才能证明**的 match 也会被拒绝（本检查不做
Maranget 的 use-tree 判定）：

```ta
match o {
  Some(Some(x)) -> 1
  Some(None) -> 2      // Some 的两个子情形都列了，仍不算「整体覆盖」
  None -> 0
}
// [E0005] cannot prove exhaustive match: nested patterns cover only part of Some
```

约定同 Gleam：要么加显式兜底臂，要么把嵌套拆成逐层 match —— 先把里层载荷绑成
变量，再在内层 match 上穷尽。逐层写法能被检查证明穷尽，因此是可静态验证的写法：

```ta
match io.read() {
  Err(e) -> fail(e)
  Ok(step) -> match step {        // Step 单变体、两个参数都是变量 ⇒ 整体覆盖
    Step(payload, rest) -> match payload {
      Both -> use(rest)
      Nope -> stop(rest)
    }
  }
}
```


### 臂的类型一致性

`match` / `receive` 的**所有臂的 body 必须同型**（与 `if` / `else` 两臂同一条规则，typecheck 层）：

```ta
fn is_none(o) {
  match o {
    Some(_) -> false
    None -> true
  }
}                      // False / True 都是 Bool，unify 成 Bool

fn bad(o) {
  match o {
    Some(_) -> false
    None -> 0
  }
}                      // type error: cannot unify Bool with Int
```

臂的类型就是整个 `match` 的类型：`is_none` 的类型是 `Option(a) -> Bool`，
调用处（`if is_none(x) { ... }`、`is_none(x) + 1`）都按 Bool 检查，
不会静默当成未约束的类型变量放行。

### 发散表达式：除零 / 模零

`x / 0` 与 `x % 0`（字面量 Int 零除数）走[「除零 / 模零：进程死亡协议」](#除零--模零进程死亡协议)——
**永远不产生值**，因此它的类型是**底类型**（bottom，内部记作 `Never`）：
与任何类型都能 unify，且**不约束**对面的类型。这正是 fail-fast 惯用式能过 typecheck 的原因：

```ta
// lib/result.ta unwrap / lib/option.ta unwrap 的形状：
fn unwrap(r) {
  match r {
    Ok(v) -> v
    Err(_) -> 1 / 0    // 崩溃臂不参与臂类型推断 → unwrap : Result(a, b) -> a
  }
}
```

`1.0 / 0.0` 是 IEEE inf（不死），所以除数必须是 Int 字面量才是底类型；
`x / y`（变量除数）仍是普通 `Int` 运算。

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

// 字段带标签：`name : type` —— 标签是文档，字段本身是 `:` 后面的类型
type Rational { R(num : Int, den : Int) }

// 公开类型（跨模块可见）
pub type Msg { Ping(Pid); Pong; Stop }
```

字段一律是**类型**：`MkPair(a, b)` 的两个字段是类型变量（隐式多态），
`R(num : Int, den : Int)` 的两个字段是 `Int`。`name :` 标签只是给读者看的，
不进入 AST，也不进入字段列表——`R(num : Int, den : Int)` 与 `R(Int, Int)` 等价
（见[「parser 生成的 AST」](#parser-生成的-ast)）。因此标签既不影响 arity 计数，
也不影响字段类型检查。


### 变体在运行时的表示

| 变体类型 | 运行时表示 | 示例 |
|---------|-----------|------|
| 零参 | 符号值 | `Red` → `'Red` |
| 带参 | 构造器调用（非柯里化函数） | `Some(42)` → 构造出 `('Some 42)` 的 Pair 结构 |

### 构造器调用：arity 与字段类型

构造器的 shape（字段个数、字段类型）在类型声明处已知，因此**不需要任何标注**就能检查：

```ta
type Point { Point(x, y, z) }

Point(1, 2)         // [E0003] wrong number of arguments for 'Point' (got 2, want 3)
Point(1, 2, 3, 4)   // [E0003] ... (got 4, want 3)
```

arity 精确到两个方向，因为 desugar 是 `(cons (quote Point) args)`：
构造器**不是**柯里化函数——`Point(1)` 不是"部分应用"，运行期只会得到畸形 Pair `('Point 1)`，
所以少给参数与多给参数一样是编译错误。字段类型同样按声明检查（包括变体名与类型名同名的声明）。

带标签的字段同理，标签只影响可读性：

```ta
type Rational { R(num : Int, den : Int) }

R(1, 2)      // ok
R(1)         // [E0003] wrong number of arguments for 'R' (got 1, want 2)
R(1, "s")    // [E0001] arg 2 of R: cannot unify String with Int
```


### parser 生成的 AST

```ta
type Color { Red; Green; Blue }
→ (type Color (quote Red) (quote Green) (quote Blue))

type Option { None; Some(value) }
→ (type Option (quote None) (Some value))

pub type Msg { Ping(Pid); Pong; Stop }
→ (type Msg (Ping (quote Pid)) (quote Pong) (quote Stop))

type Rational { R(num : Int, den : Int) }
→ (type Rational nil (R Int Int))          // 标签不进入 AST
```

---

## 类型注解

### 函数参数和返回值注解

```ta
fn add(x: Int, y: Int) -> Int {
  x + y
}
```

### parser 行为

- 注解存在时，parser 额外发出 `(type-sig name (param_types...) ret_type)` 表单
- 无注解的函数不发出 `type-sig`，AST 与之前完全一致
- `type-sig` 被代码生成器跳过（不影响编译）

```ta
// 带注解
fn add(x: Int, y: Int) -> Int { x + y }
→ ((type-sig add (Int Int) Int) (define (add x y) (+ x y)))

// 无注解
fn add(x, y) { x + y }
→ ((define (add x y) (+ x y)))
```

### 支持的注解类型

**命名规则（无例外）**：类型位置上的**首字母大写**标识符是**具体类型**——内建类型或已声明的
ADT 名；**首字母小写**标识符是**类型变量**（隐式多态，可被推导/实例化为任意类型）。

- 具体类型：内建 `Int`、`Float`、`String`、`Bool`、`Symbol`、`Pid`、`List`、`Pair`、
  `Nil`、`Resource`（C 模块不透明资源值，见
  [「资源值的传递限制」](#资源值的传递限制resource)），以及用户声明的 ADT 名（如 `Color`）。
- 类型变量：`a`、`b`、`t` 等小写名——`fn map(f: fn(a) -> b, xs: List(a)) -> List(b)` 里
  `a`/`b` 是多态变量，`List` 是具体类型构造子。

复合类型注解：支持泛型 ADT 应用，如 `List(Int)`、`Result(Int, String)`、`Option('a)`。

### 函数类型注解

高阶参数有两种拼写，指向**同一个类型**：

| 拼写 | AST | 说明 |
|------|-----|------|
| `a -> b` | `('arrow a b)` | 右结合：`a -> b -> c` = `('arrow a ('arrow b c))` |
| `fn(A, B) -> R` | `('arrow A ('arrow B R))` | 括号拼写，脱糖成同样的 curried arrow |

```ta
fn apply(f: fn(Int, Int) -> Int, a: Int, b: Int) -> Int { f(a, b) }
fn map(f: fn(a) -> b, xs: List(a)) -> List(b) { ... }        // 等价于 f: a -> b
fn pick(flag: Bool) -> fn(Int, Int) -> Int { if flag { add } else { mul } }
```

`fn(A, B) -> R` 只是把 arity 写进拼写：TA 没有部分应用，它和 `A -> B -> R` 是
同一个类型（把 `fn(Int) -> Int` 传给 `fn(Int, Int) -> Int` 参数仍报
`cannot unify Int -> Int with Int -> Int -> Int`）。脱糖发生在 parser，
typecheck / codegen 只认 `('arrow A B)` 一种函数类型形状。

**调用点实参个数**：直接调用一个用户 `define` 时，实参个数必须与声明一致——多传、
少传都报 `[E0003] wrong number of arguments for 'f' (got N, want M)`。`A -> B -> R`
表达不了「必须吃满两个实参」（`f(1)` 作为 `Int -> Int` 值本身合法），所以这个数字
取自 define 的参数表，而不是数类型里的箭头（与 `builtin_arity` 同为「符号 → arity」
表，issue #376）。把函数**当值**传给高阶函数、再在其内部以别的个数调用，typecheck
看不到（被调者只是一个参数），此动态路径由 VM `CALL` 的 fn_args 检查响亮 fatal：
`tavm: arity mismatch`，不会静默读错位槽（见 `test/crash/arity-mismatch.ta`）。

类型位置的 `fn` 必须同时给出参数表和结果：

| 写法 | 结果 |
|------|------|
| `fn(A, B) -> R` | ✅ |
| `fn -> Int` | ❌ `'fn' in a type needs a parameter list`（此前 `fn` 会被读成类型名，而裸小写名是隐式类型参数，签名被首个调用点悄悄单态化）|
| `fn()` | ❌ `fn() with no parameters is not a type`（没有 unit 类型可当零参函数的结果；零参**定义**另说）|
| `fn(A, B)` | ❌ `function type needs a result` |

括号分组 `(Int -> Int) -> Int` 尚不支持（parser 把 `(` 读成 `(Name args)` 复合类型），
需要时写 `fn(Int -> Int) -> Int`。

---

## Actor 模型

### 进程原语

| 操作 | 语法 | 说明 |
|------|------|------|
| 创建进程 | `spawn('fn_name)` 或 `spawn(fn { ... })` | 返回 Pid |
| 发送消息 | `send(pid, msg)` | 异步，消息深拷贝 |
| 接收消息 | `recv()` | 阻塞，取邮箱下一条消息 |
| 接收（带超时）| `recv_after(ms)` | 至多等 `ms` 毫秒，超时返回 `nil`（邮箱不动）|
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

### 资源值的传递限制（Resource）

`Resource` 是 C 模块的不透明资源值（模块侧形状与 dtor 约定见
[`docs/c-module.md`](c-module.md) §5）：无构造子、不可 `match`、不可比较
（相等性按 header 指针恒等）、打印 `<resource>`、没有 `resource?` 谓词。
它在 `send` / `spawn` 时**移交归属**（接收方成为 owner，不是复制出第二份
资源），因此有三条硬限制：

1. **不得 send 给自己**——对自己移交是空操作，而 GC 不扫邮箱，自己堆里的旧
   副本会被当垃圾释放，邮箱 fragment 还持着活引用；
2. **不得进定时消息**——`timer.send_after` / `timer.send_interval` 的消息体
   带资源时，注册快照与延迟投递之间没有归属可言；
3. **不得走 selective receive**——peek 分支会让「自己堆一份 + 自己邮箱
   fragment 一份」两处 `owner == self` 同时存活。

违反任一条 = **进程隔离错误**：当前进程带原因死亡（与
[「除零 / 模零：进程死亡协议」](#除零--模零进程死亡协议)同款），不会静默
丢消息、也不会返回 nil / 0。发送给已死进程按普通语义丢弃：资源留在发送方，
由发送方后续 GC / 死亡释放。

### recv_after：带超时的接收

```ta
let msg = recv_after(1000)   // 至多等 1 秒
match msg {
  nil -> { /* 超时：邮箱原封不动，消息不丢 */ }
  _   -> { /* 收到消息，同 recv() */ }
}
```

语义对齐 Erlang/Gleam：

- **消息优先**：deadline 前有消息则返回消息（FIFO，同 `recv()`）；恰好同时发生时消息赢
- **超时不消费**：超时返回 `nil`，邮箱内容与顺序不变，下次接收仍能看到
- `ms` 是 Int 表达式（可用变量）；`ms = 0` 表示只探测当前邮箱
- 超时精度受调度器轮询粒度限制（约 1–100ms 额外延迟）

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
import tokenizer       // 裸名糖：发行版 vendor 树 lib/ 下（lib/tokenizer.ta 或 lib/bootstrap/tokenizer.ta）
import msg             // 导入 lib/msg.ta
import foo as f        // 别名：本文件内写 f.member，写 foo.member 报错
import "msg"           // 字符串形：与 import msg 等价（名字是字符串）
import "bootstrap/tokenizer"                    // 子路径糖：vendor 树 lib/bootstrap/tokenizer.ta
import "example.org/hello:lib/greet"            // identity 形态：机械拼接 vendor/example.org/hello/lib/greet.ta
import "github.com/tiancaiamao/tinyactor:lib/json" as j
```

模块名也可以写成字符串（`import "msg"`）。名字里因此能出现标识符写不出的
字符（`:` `/` `.`），这是 identity 形态的载体 —— 解析规则本身与拼写形式无关。

**identity 形态** `import "<identity>:<path>"` 的唯一解析机制是机械拼接
`vendor/<identity>/<path>.ta`：没有搜索、没有注册表、没有第二条路径。
找不到时报 `error: module not found: <identity>:<path>` —— 按写出名报，
不补 `.ta`（identity 是身份不是路径）。**语言不知道「版本」**：冲突解决 /
fetch / lock 是用户与工具的事。契约全文（含否决史）见
[modules.md](modules.md)。

**裸名/子路径是标准库糖**：`"aaa"` ≡
`"github.com/tiancaiamao/tinyactor:lib/aaa"`。标准库就是普通包，发行版预装
其 vendor 目录（仓库内是符号链接 `vendor/github.com/tiancaiamao/tinyactor →
仓库根`）；fork = 覆盖 vendor 目录，没有保留规则。没有 `as` 时别名取名字的
末段（`…:lib/json` → `json`），所以正文写 `json.parse`；identity 全名在
源码里写不出来，需要短名时写 `as j`。

**卫生**：段禁空、禁 `..`；identity 首段必须含点。违规响亮拒绝：
`error: invalid module name: <name>`。

模块解析路径（裸名/子路径，按序）：

1. `{导入方所在目录}/{name}.ta`（本地模块优先）
2. `{导入方所在目录}/helpers/{name}.ta`（测试/辅助模块约定）
3. `vendor/github.com/tiancaiamao/tinyactor/lib/{name}.ta`（标准库）
4. `vendor/github.com/tiancaiamao/tinyactor/lib/bootstrap/{name}.ta`（编译器自身源码；编译器测试需要导入 tokenizer/parser）

identity 形态只走 `vendor/<identity>/<path>.ta` 一步。

导入图按解析后的路径去重（菱形依赖只加载一次），并显式检测环：
`a -> b -> a` 报 `error: circular import: a -> b -> a`（取代旧的深度 >16 启发式）。

### pub 导出（Phase B，issue #96）

```ta
// pub fn：其他模块可通过 module.fn() 调用
pub fn tokenize(src) { ... }

// 非 pub fn：仅模块内部可见（裸名调用），外部 module.fn 引用报错
fn parse_expr(toks) { ... }

// pub type：其他模块可使用该 ADT 的变体（类型与构造子保持全局裸名，Phase C 再命名空间化）
pub type Msg { Ping(Pid); Pong; Stop }

// const：没有 pub const 语法，const 一律视为导出，可用 mod.NAME 引用
const LIMIT = 10
```

编译期实现（整程序单 .tabc，零 C 改动）：driver 在 resolve 阶段把每个
模块单元的定义重命名——pub fn 为 `mod.name`，非 pub fn 为 `mod$name`
（`$` 不在标识符字符集内，用户语法无法产生，因此对外不可见），const 为
`mod.NAME`；模块内部的裸引用同步重写（driver 的 `qualify_expr` 复用
lower-ast `resolve_consts` 的作用域纪律：lambda 参数 / let 绑定 /
match·receive 模式绑定遮蔽成员名；quoted 符号仅 const 引用被重写）。
typecheck 对 dotted 调用按导出表报错：

- 成员存在但非 pub → `[E0004] 'x' is not exported by module 'm'`
- 成员不存在 → `[E0004] module 'm' has no export 'x'`

旧的“剥掉模块前缀再查裸名”fallback 已删除：两个模块导出同名裸函数不再
互相遮蔽（见 `test/module/same-name-exports.ta`）。

### import 别名

```ta
import tokenizer as tok

fn main() {
  tok.tokenize("hi")    // 唯一合法写法
  tokenizer.tokenize("hi")  // 编译期报错：real name 被别名占用
}
```

别名只作用于声明它的文件；`as` 之后的成员引用在编译期重写为真名。

### 调用导入的函数

```ta
import math

fn main() {
  print(math.abs(-42))      // module.function() 语法
}
```

### external fn（C 模块声明）

C 模块（`lib/<mod>.dylib` / `lib/<mod>.so`）的类型写在模块自己的声明文件
`lib/<mod>.ta` 里，用 `external fn` 声明 —— 只有名字、参数类型和返回类型，
没有函数体：

```ta
external fn demo.double(Int) -> Int                // 位置形式：只写类型
external fn demo.pair(a : Int, b : Int) -> Pair    // 命名形式：参数名 : 类型
```

两种写法**语义完全相同**：参数名只给读代码的人看，类型才是 call site 的检查
依据，所以 `acc + demo.double(i)` 在两种写法下都按 `Int -> Int` 检查。parser
对二者产出同一个 AST：

| 源码 | AST |
|------|-----|
| `external fn demo.double(Int) -> Int` | `(external_fn demo.double (Int) Int)` |
| `external fn demo.double(n : Int) -> Int` | `(external_fn demo.double (Int) Int)` |

参数位置上的**裸名字**（不带 `:`）是类型而不是参数名，语义是无约束类型变量
（与注解里的 `'a` 同义）。C 侧收任意值时就这么写：

```ta
external fn buffer.raw_push_str(h, s) -> Int   // h、s 都是任意类型
```

省略返回类型等于返回 `nil`（`external fn foo.bar(Int)` 等价于
`external fn foo.bar(Int) -> nil`）。宿主内置模块（`net` / `timer` / `cov` /
`num` / `vm`）没有 TA 源层，`import` 它们是 no-op，签名由 dylib 背书而非
`external fn`；`lib/<mod>.ta` 只声明，`import <mod>` 时才参与类型检查。

---

## 内置函数


### 数据操作

| 函数 | 说明 |
|------|------|
| `cons(a, b)` | 构造 Pair |
| `car(p)` | Pair 的 car（`nil` 不是 Pair，见下） |
| `cdr(p)` | Pair 的 cdr（同上） |
| `null?(x)` | 是否为 nil |
| `pair?(x)` | 是否为 Pair |
| `int?(x)` | 是否为整数 |
| `string?(x)` | 是否为字符串 |
| `symbol?(x)` | 是否为符号 |
| `print(x)` | 打印值 |

`car`/`cdr` 只接受 Pair —— **`Nil` 不是 `Pair`**（issue #236）。能静态证明为 `Nil` 的实参
（`nil` 字面量，或类型解析为 `Nil` 的值）在编译期就被拒（E0001「cannot unify Nil with
Pair」）；只有运行期才出现的 `nil` 由 VM 以 `cartype`/`cdrtype` 隔离死亡（CRASH 报告带
栈回溯）。历史上 `car(nil)` 静默返回 `nil`，于是 `if car(x) == 1 { ... }` 在 `x` 为 `nil`
时恒走 else，错误在离根因很远的地方才表现出来。取嵌套列表的第 k 个元素请用 `nil` 安全的
`list_ref`/`list.nth`，不要写 `car(cdr(cdr(x)))`。

### 字符串（str 模块）

| 函数 | 类型签名 | 说明 |
|------|----------|------|
| `str.length(s)` | `String -> Int` | 字符串长度 |
| `str.concat(a, b)` | `String -> String -> String` | 拼接 |
| `str.eq(a, b)` | `String -> String -> Bool` | 比较 |
| `str.char_at(s, i)` | `String -> Int -> Int` | 第 i 字符的 ASCII 码（-1 越界） |
| `str.substr(s, start, len)` | `String -> Int -> Int -> String` | 子串 |
| `str.to_int(s)` | `String -> Int` | 解析为整数（失败返回 0） |
| `str.from_int(n)` | `Int -> String` | 整数转字符串 |
| `str.index_of(s, sub)` | `String -> String -> Int` | 查找子串（-1 未找到） |
| `str.to_sym(s)` | `String -> Symbol` | 字符串转符号 |
| `str.sym_to_str(sym)` | `Symbol -> String` | 符号转字符串；运行时收到非 Symbol（仅可能经 FFI 边界进入）以 `'symtype` 原因杀死当前进程，不再静默返回 nil（#101） |

### 列表（via Pair）

列表是 `nil` 结尾的嵌套 Pair。没有内置 List 类型，用 `cons` + `nil` 构建：

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
[1, "a"]         // type error: cannot unify Int with String
["kernfuzz", true]  // type error: cannot unify String with Bool
```

手写 `cons(h, t)` 保持异构 Pair 语义（`a -> b -> b`），不受此规则约束——
`cons` 是原始 Pair 构造器，标准库大量依赖 `('tag . payload)` 结构。

---

## 自托管编译器

### 标准库（lib/）

| 文件 | 职责 |
|------|------|
| `lib/bootstrap/tokenizer.ta` | 词法分析 |
| `lib/bootstrap/parser.ta` | 语法分析 → AST |
| `lib/bootstrap/typecheck.ta` | HM 类型推导 + ADT + 注解检查 |
| `lib/bootstrap/compile.ta` | 代码生成（compile AST → 字节码指令序列） |
| `lib/bootstrap/lower-ast.ta` | 对接层：TA parser 的 AST（form 树）→ kl 的 `Top`（`compile` 的入参类型） |
| `lib/bootstrap/ast.ta` | kl 的类型化 AST（`Const`/`Expr`/`Top`）+ form ↔ Top 的双向桥（`.kl` 文本侧） |
| `lib/bootstrap/bytecode.ta` | 字节码落盘（`write_bc`）+ lisp 侧库文本（`lib_src`） |
| `lib/bootstrap/driver.ta` | 编译驱动：tokenize → parse → typecheck → compile（resolve_imports / typecheck_cached）；`tinyactor build` / `fmt` 分发 |
| `lib/math.ta` | 数学工具函数 |
| `lib/msg.ta` | actor 消息类型定义 |
| `lib/buffer.ta` | 可变字节缓冲区 |
| `lib/fs.ta` | 文件系统辅助（原生 file 模块之上的公共 API） |
| `lib/strconv.ta` | Int/Float ↔ String 转换 |

### Bootstrap

```bash
make tinyactor                     # 构建 C VM
./tinyactor build lib/math.ta /tmp/math.tabc  # 用 TA 编译器编译出字节码镜像
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

### 类型错误报告（硬错误）

类型检查在每次编译（`run` / `build`）时自动执行，发现类型错误即中止编译，不需要任何额外 flag：

```bash
NWORKERS=1 ./tinyactor run source.ta
```

类型检查器会推导所有函数类型，验证注解，并报告不匹配错误：

```
typecheck: 3 type error(s) found
  [E0001] in function 'main' (line 13): arg 1 of bad_if: cannot unify Int with Bool
           line 14: >>>   bad_if(1)
  [E0001] in function 'bad_if' (line 1): cannot unify Int with Bool
lisp-backend: typecheck
```

错误信息包含出错的函数名、行号和类型冲突详情；存在任一类型错误时编译以非零退出码中止，不生成字节码。（`--check` 是 `tinyactor fmt --check` 的格式检查 flag，与类型检查无关。）

### 注解强制执行

当函数声明了类型注解时，类型检查器会：

1. **注册声明类型**：`fn f(x: Int) -> Int` 注册 `Int -> Int` 作为期望类型
2. **推导实际类型**：从函数体推导实际类型
3. **统一检查**：如果两者不匹配，报告类型错误

未注解的函数不受影响——它们照常推导但不会与声明类型比较。

### 内建函数类型签名

类型检查器内置了常用函数的类型签名，无需注解即可正确推导：

| 分类 | 函数 | 类型 |
|------|------|------|
| 算术 | `+` `-` `*` `/` | `Int -> Int -> Int` |
| 比较 | `<` `>` `<=` `>=` | `Int -> Int -> Bool` |
| 相等 | `==` | `forall a. a -> a -> Bool` |
| 布尔 | `not` | `Bool -> Bool`（注：`not` 是 `lib/bool.ta` 的普通函数，签名来自其注解，不在内建承诺表） |
| 列表 | `car` `cdr` | `forall a b. a -> b`（元素类型不约束；可证明的 nil 实参编译期拒绝，见「内置函数」） |
| 构造 | `cons` | `forall a b. a -> b -> b` |
| 谓词 | `null?` `pair?` `int?` `string?` `symbol?` `bytes?` `pid?` | `forall a. a -> Bool` |
| 字符串 | `str.concat` `str.eq` `str.length` 等 | 见上方字符串函数表 |
| Actor | `spawn` `self` | `forall a. a -> Pid` |
| 消息 | `send` | `forall a b. a -> b -> b` |
| 消息 | `recv` | `forall a. a` |

---

## 类型系统当前能力边界

### 能力

| 能力 | 状态 | 示例 |
|------|------|------|
| 整数运算推导 | ✅ | `fn f(x) { x + 1 }` → `Int -> Int` |
| 多态推导 | ✅ | `fn id(x) { x }` → `'a -> 'a` |
| 高阶函数 | ✅ | `fn app(f, x) { f(x) }` → `('a -> 'b) -> 'a -> 'b` |
| 递归函数 | ✅ | `fn fact(n) { ... }` → `Int -> Int` |
| ADT 变体 | ✅ | `Red` → `Color` |
| 泛型 ADT | ✅ | `type List { Nil; Cons(a, List(a)) }` → `List(Int)` |
| Actor 原语 | ✅ | `spawn` → `'a -> Pid` |
| 函数注解验证 | ✅ | `fn add(x: Int, y: Int) -> Int` 匹配推导 |
| 复合类型注解 | ✅ | `fn f(xs: List(Int)) -> Int` 验证参数类型 |
| 函数类型注解 | ✅ | `fn f(g: fn(Int) -> Bool)`，等价于 `g: Int -> Bool` |
| 类型错误报告 | ✅ | 编译中止并输出 `[E0001] in function 'foo': cannot unify Int with String` |
| 内建函数签名 | ✅ | `str.from_int` → `Int -> String`，`+` → `Int -> Int -> Int` |

### 不支持

| 缺失 | 说明 |
|------|------|
| List 类型 | 没有 `List(a)` 内置类型，nil/cons 组合只推导为 Pair + tvar，无法区分空列表和空值 |
| 函数类型括号分组 | 不能写 `(Int -> Int) -> Int`（`(` 被读成 `(Name args)`）；写 `fn(Int -> Int) -> Int` 或 `Int -> Int -> Int` |

### nil 的类型问题

`nil` 在类型系统中推导为 fresh tvar（`'b`），既是空值又是空列表。HM 无法区分：
- `fn f() { nil }` → `'a -> 'b`（返回值是什么类型？任意）
- `fn make_list() { cons(1, nil) }` → `'a -> 'b`（应该是 `Int List`，但推导不出）

对比 Gleam：没有 `nil`，用 `Result(a, b)` / `Option(a)` 代替，List 有明确的 `List(a)` 类型。

---

## 限制与约束

- **没有宏系统**（不像 Cora/Lisp 有 defmacro）
- **没有可变状态**（没有 set!/ref/mutable）
- **没有数组/向量**（只有 Pair 和 nil）
- **没有逻辑非 `!`**（`&&` / `||` 支持且短路求值；取反用 `x == false` 或嵌套 `if`）
- **顶层只能定义函数、类型、import**（不能有顶层表达式）
- **不支持函数类型的括号分组**（`(Int -> Int) -> Int`；写 `fn(Int -> Int) -> Int`）
- **类型检查为硬错误**（存在类型错误时编译中止，报告含 `[E00xx]` 错误码、函数名与行号位置）