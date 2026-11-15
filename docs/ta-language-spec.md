# TinyActor (TA) 语言规范

## 1. 语言画像

- **设计**：Erlang 风格的 actor 模型 + ML/Rust 系语法。编译器自身用 TA 编写（自举）。
- **求值**：严格求值。函数调用在尾位置有 TCO（尾调用不增长栈）。
- **类型**：Hindley-Milner 类型推导 + 可选注解；类型错误是编译期硬错误（`[E00xx]` 错误码，编译中止）。
- **错误**：运行期错误（除零、类型隔离死亡）杀死**当前进程**，其余进程不受影响（进程隔离）。

所有值不可变，没有可变状态；唯一的状态是进程邮箱。没有数组、没有可变引用、没有宏。

编译管线（tokenizer → parser → typecheck → lower → codegen → VM 字节码）见
[`ta-compilation.md`](ta-compilation.md)，本文只描述语言本身的可观察行为。

---

## 2. 词法结构

### 标识符

标识符以字母或 `_` 开头，后续字符可以是字母、数字、`.` `+` `-` `<` `>` `=` `!` `?` `*` `/`。
注意 `+` `-` `<` `>` 等在标识符字符集里，因此 `a<>b` 会被读成**一个标识符**，
`a + b` 两侧必须留空格（字面量旁例外，见「常见错误」）。

### 关键字

```
fn let match if else when spawn send import receive pub type external const nil true false
```

`use` **不是**关键字：仅当后跟 `<-` 时才按 use 语法解析（见 §4.3），`use(x)` 是普通调用。

### 字面量

| 形式 | 类型 | 说明 |
|------|------|------|
| `42` `-3` | Int | 十进制整数，字面量即按 int48 回绕归一化（§6.1） |
| `3.14` `-0.5` | Float | 必须是 `数字.数字` 两侧都有数字；不支持指数记法、`1.`、`.5` |
| `"hello"` | String | 转义 `\n \r \t \\ \" \'`、`\xNN` 十六进制字节；未知转义保留原样 |
| `'foo` | Symbol | 符号字面量（单引号前缀），用于 ADT 零参变体、消息标签 |
| `true` `false` | Bool | — |
| `nil` | Nil | 空值 / 空列表 |

### 注释

`//` 到行尾。token 层不保留空白与注释信息（`< -` 无法区分 `<-`，见 §4.3）。

---

## 3. 值与类型

### 值类型表

| 类型 | 产生方式 | 说明 |
|------|----------|------|
| Int | `42`、算术 | 48 位有符号整数，语义见 §6.1 |
| Float | `3.14`、算术 | IEEE 754 double，语义见 §6.3 |
| String | `"…"`、`str.*` | 不可变 |
| Bool | `true`/`false`、比较 | — |
| Symbol | `'foo`、`str.to_sym` | 不可变 |
| Nil | `nil` | 空值与空列表 |
| Pair | `cons(h, t)` | 有序二元组，列表的基本单元（§4.7） |
| Tuple | 构造子调用 `Point(1, 2)` | 定长，`tuple-ref` O(1)；带参 ADT 构造子的值形态（§4.8） |
| 闭包 | `fn(x) { … }`、`fn { … }` | 可捕获局部变量 |
| Pid | `spawn(…)` | 进程标识符 |
| Resource | C 模块 API 产出 | 不透明资源值，传递限制见 §8.4 |

### 列表

列表是 `nil` 结尾的嵌套 Pair，用 `cons` + `nil` 构建，或写 `[a, b, c]` 字面量（§4.7）。

---

## 4. 核心语法

### 4.1 函数定义 `fn`

| 形式 | 说明 |
|------|------|
| `fn f(x, y) { body }` | 命名函数 |
| `pub fn f(x) { body }` | 公开函数，跨模块可见（§9.2） |
| `fn f(x: Int) -> Int { body }` | 带类型注解（§7） |
| `fn(x) { … }` / `fn { … }` | 匿名函数，**只能出现在表达式位置**（let 值、调用实参）；顶层出现是 parse error |
| `spawn('worker)` | 按符号名引用顶层函数（等价 `fn { worker() }`；名字拼错则运行期显式失败） |

函数是一等值：可以绑定、传递、返回。TA 没有部分应用——`f(a)` 是完整调用，
`fn f(x, y) { … }` 必须一次给齐参数（§7.4）。

### 4.2 变量绑定 `let`

```ta
let x = 42
let y = x + 1     // 后续绑定可引用前面的
```

- let 是函数体内的顺序绑定；后面的 let 可引用前面的，反向不行
- 顶层只能定义函数、类型、import（以及 const，见 §9.2），**不能**有顶层 let 或顶层表达式
- 局部同名遮蔽全局：形参 / let / match / receive 的绑定与某个 define 同名时，
  范围内是局部值

### 4.3 use 绑定（do-notation）

块首的 `use x <- expr` 把块内其余部分作为延续传给 expr：

```ta
use x <- expr
rest...
// 等价于：expr(fn(x) { rest... })
```

规则：

- 必须位于**块首**（`let` 之后亦可），可链式；块中间出现是 parse error
- `use <- expr` 丢弃结果（绑定到 `_`）
- expr 必须返回**吃延续的函数**：`use x <- 42` 是 type error（`cannot unify Int with 'a -> 'b`），
  不是运行时崩溃
- 库用包装函数适配：mpc 的 `use v <- mpc.then(p)` 中，`mpc.then(p)` 返回 `fn(k) { bind(p, k) }`
- `<-` 由 `<` 与 `-` 两个 token 组成，`use < -x` 与 `use <-x` 无法区分，一律按绑定解析

示例（mpc 库）：

```ta
fn parse_two(s, i) {
  use k <- mpc.then(take_char('a'))
  use v <- mpc.then(take_char('b'))
  mpc.ret(cons(k, cons(v, nil)))
}
```

### 4.4 控制流

```ta
if x > 0 {
  print("positive")
} else {
  print("non-positive")
}
```

- 条件必须是 Bool，否则 type error
- **没有 else 时 else 分支为 nil**：`if done { print("done") }` 等价于带 `else { nil }`
- `{ }` 是块表达式：顺序执行，**返回最后一个表达式的值**
- if/块是表达式，不是语句

### 4.5 模式匹配 `match`

```ta
match scrutinee {
  pattern -> expr
  pattern when guard -> expr    // guard 为 false 时跳过该臂，尝试下一个
  _ -> default
}
```

- 按臂顺序尝试，**首个**命中且 guard 为真的臂生效
- 无臂命中时 match 静默求值为 nil（ADT / List 的穷尽性检查会拦掉大部分漏臂，§4.5.2）
- guard 可引用 pattern 绑定的变量，支持 `&&` / `||`

#### 4.5.1 模式语法

| 模式 | 匹配 | 示例 |
|------|------|------|
| 整数字面量 | 精确匹配 | `42 ->` |
| 符号字面量 | 精确匹配 | `'hello ->`、`'+ ->` |
| `nil` / `true` / `false` | 精确匹配 | `nil ->` |
| 裸符号（小写） | 变量绑定，匹配任何值 | `n ->` |
| `_` | 通配符 | `_ ->` |
| `Name`（大写、无参） | 零参构造子 | `Red ->` |
| `Name(a, b)` | 构造子：标签相等 + 逐参解构 + 定长钉死 arity | `Ok(v) ->` |
| `mod.Name(a, b)` | 跨模块构造子 | `option.Some(v) ->` |
| `cons(a, b)` | Pair 解构：a 绑 head，b 绑 tail，不限长度 | `cons(h, t) ->` |
| `[a, b, c]` | 定长列表：长度必须恰好为 3 | `['DOWN, r, pid, reason] ->` |
| `[a, b \| rest]` | 前 2 个定长 + 活尾巴（匹配长度 ≥ 2） | `[h \| t] ->` |

- **大写**构造子模式按标签严格相等判定；`mod.Name` 看的是**最后一个点段**（大写 =
  构造子，小写 = 变量/调用，`bufio.read(v)` 不构成模式）
- 其余 token（裸算符、`{`、`,` …）不构成模式，是 parse error——不会静默降成 `_`
- 定长方括号臂（`[a]`、`[Bind(k, v), _]`）**不吸收多余元素**：3 元素链表遇
  `[a, b]` 不匹配——要遍历用 `[h | t]` 或 `cons(h, t)`

#### 4.5.2 穷尽性检查

编译器对 ADT match 与 `List(E)` match 做穷尽性检查：缺变体且无兜底臂（变量/`_`/
活尾巴）时编译拒绝：

```
[E0005] non-exhaustive match: missing Blue
```

- 有意部分匹配需显式写 `_ ->` 臂
- 嵌套构造子只算**部分覆盖**，报
  `[E0005] cannot prove exhaustive match: nested patterns cover only part of Some — add an explicit _ -> arm`
  ——检查是充分条件，真正穷尽但需交叉推理的 match 也会被拒；把里层绑成变量逐层
  match 是可静态验证的写法
- `List(E)` 的两个形态是 `nil` 与 `cons`：`nil ->` / `[] ->` 与 `cons(h, t)` /
  `[h | t]`（tail 必须是活的变量/通配符）
- **未知构造子**：拼错的变体名（`Grn` 之于 `Green`）是编译错误
  `[E0005] unknown constructor 'Grn' in match on Color`，而不是静默死臂
- `receive` 不做穷尽性检查（邮箱内消息类型混合，无单一 scrutinee 类型）

### 4.6 运算符

| 运算符 | 说明 |
|--------|------|
| `+` `-` `*` `/` | 算术，严格同型（§6） |
| `%` | 取模，仅 Int |
| `==` `!=` | 相等，同型比较（§6.2）；与 `nil` 比较豁免 |
| `<` `>` `<=` `>=` | 比较，严格同型，结果 Bool |
| `&&` `\|\|` | 逻辑与/或，短路求值；可用于 guard 与普通表达式 |
| `<>` | 字符串拼接，见下 |
| `\|>` | 管道，见下 |

**没有逻辑非 `!`**。取反用 `x == false` 或嵌套 `if`。

#### 字符串拼接 `<>`

`<>` 是 `str.concat` 的中缀写法：

```ta
"abc" <> "def"             // => "abcdef"
"n=" <> str.from_int(42)   // 操作数可以是任意表达式
```

- 操作数必须是 String：`1 <> 2` 报 `arg 2 of str.concat: cannot unify Int with String`
  （报错里是脱糖后的名字 `str.concat`，不是 `<>`）
- 与算术同优先级、左结合：`a <> b <> c` = `(str.concat (str.concat a b) c)`
- 反复拼接大字符串请用 `buffer`（`lib/buffer.ta`），`<>` 链是 O(n²)

#### 管道 `|>`

`|>` 把左侧值作为**第一个参数**传给右侧：

```ta
5 |> double()        // => double(5)
x |> f               // => f(x)            // 裸函数名
5 |> add(3)          // => add(5, 3)
5 |> f() |> g()      // => g(f(5))         // 左结合
```

- **最低**优先级（比 `||` 还松）：`a + b |> f()` = `f(a + b)`
- 右侧必须是**函数调用**、**裸函数名**或**构造函数调用**；字面量、中缀表达式、
  `{ }` 块、`if`/`let`/`match`、lambda 都是 parse error（`5 |> 42`、`5 |> 1 + 2`）
- 约定尚未统一：注首参只对「数据在前」的函数顺手（`str.split` / `dict.get` 等），
  `list` / `option` / `mpc` 的 HOF 是「函数在前」，写不出 `orders |> list.map(fn)`。
  收敛方案见 `docs/pipe-arg-order.md`（待决策）
- **与 `||` 的不对称**：`a || b |> c` 解析为 `c(a || b)`（`||` 结果是 Bool，
  传非 Bool 参数报 E0001，正常工作）；`a |> b || c` 是 parse error（右侧不允许 `||` 结果）

### 4.7 列表字面量

```ta
[1, 2, 3]        // List(Int)
["a", "b"]       // List(String)
[]               // 空表，无元素约束
[1, "a"]         // type error: cannot unify Int with String
```

- 列表字面量**所有元素类型必须统一**，推导为 `List(E)`
- 手写 `cons(h, t)` 是原始 Pair 构造器（`a -> b -> b`），不受统一约束——
  标准库大量依赖 `('tag . payload)` 异构结构
- 取第 k 个元素用 nil 安全的 `list.nth`（`lib/list.ta`），不要写 `car(cdr(cdr(x)))`

### 4.8 ADT（代数数据类型）

```ta
type Color { Red; Green; Blue }          // 零参变体
type Option { None; Some(value) }        // 带参变体
type Rational { R(num : Int, den : Int) } // 字段带标签（标签只是文档，不进类型）
pub type Msg { Ping(Pid); Pong; Stop }   // 跨模块可见
```

- 字段一律是**类型**：`Some(value)` 的字段是类型变量（隐式多态），
  `R(num : Int, den : Int)` 与 `R(Int, Int)` 等价
- **零参变体**的值是符号（`Red` → `'Red`）；**带参变体**的值是定长扁平 tuple
  （`Some(42)` → 第 0 槽为符号 `'Some`，其余为字段），`tuple-ref` O(1)
- 模式侧与值同形：「模式 = 值形态」，`Some(v) ->` 按标签严格相等 + 逐参解构
- 构造子 arity 双向精确：`Point(1, 2)`（want 3）报
  `[E0003] wrong number of arguments for 'Point' (got 2, want 3)`；
  构造子不是柯里化函数，不存在部分应用

#### 按字段名构造与更新

声明处写了标签，调用处就能**按名字**给参数，顺序不必与声明一致：

```ta
type Rational { R(num : Int, den : Int) }

R(3, 4)             // 位置：标签出现之前就有的写法，照旧
R(num: 3, den: 4)   // 按名字
R(den: 4, num: 3)   // 名字寻址，与声明相反的顺序，值相同

let half = R(1, 2)
R(..half, num: 5)   // 更新：拷贝 half 的全部字段，再替换点名的那些 -> R(5, 2)
```

- 一次调用要么**全是位置**参数，要么**全是标签**，混用是 parse error（`R(1, den: 2)`）
- `..base` 只出现在参数表第一位；`base` 是本构造子类型的值（否则 E0001），
  任意表达式、只求值一次；更新结果与 `base` 同类型
- 不带 `..base` 时每个字段都要给：漏报 `[E0007] 'R' is missing field 'den'`
- 标签必须声明过：写错 `[E0007] 'R' has no field 'z'`，重复
  `[E0007] duplicate field 'num' in R(...)`
- 零参变体没有字段可搬：`Red(..r)` 报 `[E0007] 'Red' has no fields to update`
- 标签**不进模式**：`R(num: 3, den: 4)` 的值就是 `R(3, 4)`，解构按位置写
- 声明里没标签的字段没有名字，`R(x: 1)` 无从对应——把声明改成 `R(x : Int)` 就有了

#### 限定构造子：类型与构造子是模块成员

类型名、构造子名与 `pub fn` / `const` 一样是**模块成员**，跨模块引用必须带模块前缀
（没有 prelude、没有全局裸名层）：

```ta
import "tinyactor:lib/option" as opt

fn unwrap(o : opt.Option(Int), d) -> Int {
  match o {
    opt.Some(v) -> v
    opt.None -> d
  }
}

opt.Some(1)              // 值位置
```

- 前缀 = import 的引用名（末段或 `as` 别名），同时作用于类型、值、模式三处
- 定义模块内部用裸名（`lib/option.ta` 自己写 `Some`），编译器重写为限定符号
- 裸名查不到绑定是编译错误，报错给出可原样贴回源码的建议：
  `[E0004] 'Some' is not in scope; did you mean 'option.Some'?`

---

## 5. 求值规则

- **严格求值**：函数实参在调用前求值
- **尾调用优化**：函数体内的尾位置调用（`if` 分支体、块尾表达式）不增长调用栈，
  无限尾递归不会爆栈（TCO 保证：500 万次迭代可用）
- **作用域**：词法作用域；函数可捕获局部变量形成闭包
- **表达式皆值**：`if` / 块 / `match` 都是表达式，可出现在任何表达式位置
- **match 求值**：按臂顺序，首个命中且 guard 为真的臂；无命中 → nil（§4.5）

---

## 6. 算术语义

### 6.1 Int：int48 回绕

整数是 **48 位二进制补码**，取值范围 **[-2^47, 2^47 - 1]**
（[-140737488355328, 140737488355327]）。

- `+` `-` `*` 溢出时**静默按模 2^48 回绕**：不报错、不饱和、不升级为浮点
  - `2^46 * 2` → `-140737488355328`；`2^46 * 4` → `0`
  - `140737488355328 == -140737488355328` 为 true（字面量同一套归一化）
- `/` `%` 遵循 **C 语义**：商向零截断，余数符号跟随被除数
  - `-7 / 2` → `-3`；`-7 % 2` → `-1`（不是 Python 的 1）
- `INT48_MIN / -1` 回绕为 `INT48_MIN`；`INT48_MIN % -1` → `0`

**除零 / 模零**（`x / 0`、`x % 0`）杀死**当前进程**（reason `'divzero`）：

- stderr 打印崩溃报告（`** CRASH pid N: 'divzero`，含函数位置）
- monitor 方收到 `['DOWN, ref, pid, 'divzero]`
- main 进程死亡 → 程序 exit code 1；子进程死亡 → 其余进程继续，exit code 不受影响
- 浮点除零不死亡（`1.0 / 0.0` → inf）

### 6.2 相等

`==` / `!=` 要求操作数**同型**：Int == Float、Bool == Int、String == Int 等
跨类型比较一律 type error。唯一豁免：**与 `nil` 的比较**永远合法
（`x == nil`、`nil != false`），nil 是单例，不约束另一侧类型。

### 6.3 严格数值塔

Int 和 Float 是完全不同的类型，任何运算符都不做隐式转换（OCaml/Gleam 式）：

```ta
1 + 2         // ok
1.5 + 2.0     // ok
3 + 0.5       // type error: cannot unify
2.5 < 3       // type error
```

- `%` 仅 Int
- Float 是 IEEE 754 double：`1.0 / 0.0` → inf，`0.0 / 0.0` → nan；
  `nan != nan` 成立、`nan == nan` 不成立
- 跨塔转换（唯一合法途径）：

| 函数 | 类型 | 语义 |
|------|------|------|
| `int.to_float(n)` | Int -> Float | 精确升宽 |
| `float.to_int(d)` | Float -> Int | 向零截断；越界按 int48 回绕；NaN 截断为 0 |

### 6.4 比较

`<` `>` `<=` `>=` 严格同型（Int~Int 或 Float~Float），结果 Bool。
字符串比较用 `str.eq`。

---

## 7. 类型系统

### 7.1 推导

Hindley-Milner 推导，每次编译自动执行，无需 flag。支持：

| 能力 | 示例 |
|------|------|
| 基本推导 | `fn f(x) { x + 1 }` → `Int -> Int` |
| 多态 | `fn id(x) { x }` → `'a -> 'a` |
| 高阶函数 | `fn app(f, x) { f(x) }` → `('a -> 'b) -> 'a -> 'b` |
| 递归 | `fn fact(n) { if n == 0 { 1 } else { n * fact(n - 1) } }` → `Int -> Int` |
| ADT 与泛型 ADT | `Red` → `Color`；`List(Int)` |
| Actor 原语 | `spawn` → `'a -> Pid` |
| 内建签名 | `+` → `Int -> Int -> Int`、`str.from_int` → `Int -> String` |

类型错误是**硬错误**：编译中止，报告含 `[E00xx]` 错误码、函数名、行号与冲突详情：

```
[E0001] in function 'main' (line 13): arg 1 of bad_if: cannot unify Int with Bool
```

### 7.2 注解

```ta
fn add(x: Int, y: Int) -> Int { x + y }
fn f(xs: List(Int)) -> Int { ... }
fn f(g: fn(Int) -> Bool) { ... }     // 等价于 g: Int -> Bool
```

- 注解存在时与推导结果统一，不匹配时错误归在函数自身（只报一次，
  调用方看到的仍是声明类型）
- 未注解函数照常推导
- **命名规则（无例外）**：类型位置首字母**大写** = 具体类型（内建或 ADT），
  首字母**小写** = 类型变量（隐式多态）
- 内建具体类型：`Int Float String Bool Symbol Pid List Pair Nil Resource Atom`，
  以及用户 ADT（跨模块写 `mod.Name`）
- 复合类型：`List(Int)`、`Result(Int, String)`、`Option('a)`

### 7.3 函数类型

`a -> b` 与 `fn(A, B) -> R` 是**同一个类型**的两种拼写（后者脱糖为 curried arrow）：

```ta
fn apply(f: fn(Int, Int) -> Int, a: Int, b: Int) -> Int { f(a, b) }
fn pick(flag: Bool) -> fn(Int, Int) -> Int { if flag { add } else { mul } }
```

| 写法 | 结果 |
|------|------|
| `fn(A, B) -> R` | ✅ |
| `fn -> Int` | ❌ `'fn' in a type needs a parameter list` |
| `fn()` | ❌ `fn() with no parameters is not a type` |
| `fn(A, B)` | ❌ `function type needs a result` |

括号分组 `(Int -> Int) -> Int` 尚不支持，需要时写 `fn(Int -> Int) -> Int`。

### 7.4 调用 arity

- 直接调用用户函数：实参个数必须与声明一致，否则
  `[E0003] wrong number of arguments for 'f' (got N, want M)`
- 函数**作为值**传递、在内部以别的个数调用时，typecheck 看不到；
  VM 侧响亮 fatal（`arity mismatch`），不会静默读错位槽

### 7.5 边界

- **`cons` 不产生 `List(E)`**：`cons` 签名是 `a -> b -> b`（返回取尾部类型），
  `cons(1, nil)` 推导为自由变量而不是 `List(Int)`——要精确类型用字面量
- **动态访问器只能写 `'a`**：`car` / `cdr` / `list.nth` 服务「pair 或原子」的
  联合形状，实参 `'a`、返回 `'b`
- **nil 推导为自由变量**：既是空值又是空列表，HM 无法区分
- **没有联合类型**：「符号或表」这种形状只能写 `'a`

### 7.6 两个逃生舱

- **`Atom`**：唯一的「我不知道类型」标注。与任何类型 unify 成功且**不绑定**任何一边；
  返回 `Atom` 的函数可以把不同形状并排放在各分支里，拿回来的值仍按真类型参与运算
  （编译期放手，不是运行期装箱）。只在「一个位置本来装多种形状、HM 没有 union 可写」
  时用；别处出现基本都是「类型没想清楚」的信号。
- **底类型（`Never`）**：`x / 0`、`x % 0`（Int **字面量**零除数）永远不产生值，
  其类型与任何类型 unify 且不约束对面。fail-fast 惯用式因此能过 typecheck：

  ```ta
  fn unwrap(r) {
    match r {
      Ok(v) -> v
      Err(_) -> 1 / 0    // 崩溃臂不参与臂类型推断 → unwrap : Result(a, b) -> a
    }
  }
  ```

  `1.0 / 0.0` 是 IEEE inf（不死）；`x / y`（变量除数）仍是普通 Int 运算。

---

## 8. Actor 模型

### 8.1 原语

| 操作 | 语法 | 说明 |
|------|------|------|
| 创建进程 | `spawn('fn)` / `spawn(fn { … })` | 返回 Pid；新进程有自己的栈和邮箱 |
| 发送消息 | `send(pid, msg)` | 异步；任何值可作消息 |
| 接收 | `recv()` | 阻塞，FIFO 取下一条 |
| 接收（超时） | `recv_after(ms)` | 至多等 ms 毫秒，超时返回 nil，邮箱不动 |
| 接收（选择性） | `receive { pattern -> body }` | 扫描邮箱，跳过不匹配的（保留） |
| 自身 Pid | `self()` | — |
| 监控 | `monitor(pid)` | 返回 ref；pid 死亡时收到 `['DOWN, ref, pid, reason]` |

```ta
spawn('worker)                     // 按名 spawn 无参函数
spawn(fn { server(config) })       // 闭包，可捕获变量
spawn(fn(x) { loop(x) })           // 匿名函数
```

### 8.2 recv vs receive

```ta
match recv() {                     // 严格 FIFO：下一条不匹配任何臂 → 进程崩溃
  'ping -> ...
  'pong -> ...
}

receive {                          // 选择性：跳过不匹配的，保留在邮箱
  'ping -> ...
}
```

### 8.3 recv_after

```ta
let msg = recv_after(1000)
match msg {
  nil -> { /* 超时：邮箱原封不动 */ }
  _   -> { /* 收到消息 */ }
}
```

- **消息优先**：deadline 前有消息则返回消息；恰好同时发生时消息赢
- 超时不消费：下次接收仍能看到
- `ms` 是 Int 表达式，`0` = 只探测当前邮箱
- 超时精度受调度粒度限制（约 1–100ms 额外延迟）

### 8.4 资源值（Resource）的传递限制

`Resource` 是 C 模块的不透明资源值：无构造子、不可 match、不可比较
（按 header 指针恒等）、打印 `<resource>`。`send` / `spawn` 时**移交归属**
（接收方成为 owner），因此三条硬限制：

1. **不得 send 给自己**（自己移交是空操作，旧副本会被 GC 释放）
2. **不得进定时消息**（`timer.send_after` / `send_interval` 的消息体带资源）
3. **不得走 selective receive**

违反任一条 = 进程隔离错误：当前进程带原因死亡，不静默丢消息。
发送给已死进程按普通语义丢弃：资源留在发送方。

### 8.5 隔离与调度

- 每个 actor 独立栈与邮箱；一个 actor 崩溃不影响其他 actor
- 抢占式调度：多线程 worker（默认按 CPU 核数），reduction 计数耗尽即切换，
  递归进程不会饿死其他进程

---

## 9. 模块系统

### 9.1 import

```ta
import "example.org/hello:lib/greet"              // 唯一形态：<树名>:<树内路径>
import "example.org/consumer:app/screen" as screen // 显式别名（可省：默认 = 末段）
import "tinyactor:lib/json" as j
```

- **名字只有一种写法**：`<树名>:<树内路径>`，省略 `.ta`。没有裸名、相对形式、
  `std:`——所以没有第二种拼写、没有候选列表
- 解析是一次机械拼接：`<根>/vendor/<树名>/<树内路径>.ta`。根 = 入口源文件向上
  第一个含 `vendor/` 的祖先目录，不读 `$PWD`、不读环境变量
- 失败只有三种（响亮拒绝）：

```
error: invalid module name: <name>           // 名字违规
error: module not found: <name>              // 拼出的路径读不到，附 tried / root
error: no vendor/ directory above <src>      // 有 import 却没有根，提示 tinyactor init
```

- **卫生**：名字必须含且只含一个 `:`；树名与路径按 `/` 切段后段全非空、
  禁 `.` / `..` 点段（点段会让同一文件有第二个名字，而名字即身份）
- **一个文件 = 一个单元**，单元键是写出的名字；菱形依赖天然只加载一次；
  环显式检测：`error: circular import: a -> b -> a`
- 语言不知道「版本」：冲突 / fetch / lock 是用户与工具的事
- 完整契约（根、身份、别名、错误信息）见 [`modules.md`](modules.md)

### 9.2 pub 导出

```ta
pub fn tokenize(src) { ... }     // 跨模块可见：mod.tokenize
fn parse_expr(toks) { ... }      // 仅模块内部（裸名调用）
pub type Msg { Ping(Pid); Pong } // 跨模块可见：mod.Msg / mod.Ping
const LIMIT = 10                 // 没有 pub const：const 一律导出，mod.LIMIT
```

- 成员存在但非 pub → `[E0004] 'x' is not exported by module 'm'`
- 成员不存在 → `[E0004] module 'm' has no export 'x'`
- 两个模块导出同名函数互不遮蔽（都须带前缀引用）

### 9.3 别名

```ta
import "tinyactor:lib/bootstrap/tokenizer" as tok
tok.tokenize("hi")          // 唯一合法写法
tokenizer.tokenize("hi")    // 编译期报错：real name 被别名占用
```

别名只作用于声明文件；成员引用在编译期重写为真名。

### 9.4 external fn（C 模块声明）

C 模块（`lib/<mod>.dylib` / `.so`）的类型写在模块自己的声明文件 `lib/<mod>.ta`：

```ta
external fn demo.double(Int) -> Int               // 位置形式
external fn demo.pair(a : Int, b : Int) -> Pair   // 命名形式（参数名只是文档）
```

- 两种形式语义完全相同：参数名只给读者看，类型才是调用点检查依据
- 参数位置上的**裸名字**（不带 `:`）是无约束类型变量（等同 `'a`）：
  `external fn buffer.raw_push_str(h, s) -> Int` 里 h、s 都是任意类型
- 省略返回类型 = 返回 nil
- 声明是**强约束**：只要模块被 import，实参逐个按声明类型合一，不符报 E0001；
  吃具体类型的原语要把类型写全（`external fn net.close(fd : Int) -> a`）
- **可选参数写成两个名字**：TA 表达不了可选参数，`net.connect`（2 参）与
  `net.connect_timeout`（3 参）是同一实现的两个固定元数注册项

---

## 10. 内置函数与标准库

### 数据操作

| 函数 | 说明 |
|------|------|
| `cons(a, b)` | 构造 Pair |
| `car(p)` / `cdr(p)` | Pair 的 car / cdr。**`Nil` 不是 `Pair`**：可证明的 nil 实参编译期拒绝（E0001）；运行期出现的 nil 以 `cartype`/`cdrtype` 隔离死亡 |
| `null?(x)` `pair?(x)` `int?(x)` `string?(x)` `symbol?(x)` `float?(x)` `bool?(x)` `tuple?(x)` | 类型谓词，返回 **Bool**（`if` 里裸用，见「常见错误」） |
| `tuple-ref(t, i)` / `tuple-len(t)` | 元组第 i 槽（越界杀死当前进程）/ 槽数 |
| `print(x)` | 打印值 |

### 字符串（str 模块，builtin，无需 import）

| 函数 | 类型 | 说明 |
|------|------|------|
| `str.length(s)` | String -> Int | 长度 |
| `str.concat(a, b)` | String -> String -> String | 拼接（**恰好 2 参**，`<>` 的底层） |
| `str.eq(a, b)` | String -> String -> Bool | 比较 |
| `str.char_at(s, i)` | String -> Int -> Int | 第 i 字符 ASCII 码（-1 越界） |
| `str.substr(s, start, len)` | String -> Int -> Int -> String | 子串 |
| `str.to_int(s)` | String -> Int | 解析（失败返回 0） |
| `str.from_int(n)` | Int -> String | 转字符串 |
| `str.index_of(s, sub)` | String -> String -> Int | 查找（-1 未找到） |
| `str.to_sym(s)` / `str.sym_to_str(sym)` | String -> Symbol / Symbol -> String | 互转（非 Symbol 进 sym_to_str 以 `'symtype` 死亡） |

### 标准库

标准库是普通包（`vendor/tinyactor/lib/`），没有保留规则。常用模块：

| 模块 | 职责 |
|------|------|
| `lib/list.ta` | nil 安全的列表操作（`list.nth` 等） |
| `lib/option.ta` / `lib/result.ta` | Option / Result ADT |
| `lib/buffer.ta` | 可变字节缓冲区（大量拼接用这个，不是 `<>` 链） |
| `lib/math.ta` `lib/msg.ta` `lib/fs.ta` `lib/strconv.ta` `lib/mpc.ta` | 数学 / 消息类型 / 文件系统 / 转换 / parser combinator |

自举流程与编译器内部文件布局见 [`ta-compilation.md`](ta-compilation.md)。

---

## 11. 常见错误

wrong / correct 成对。

### 谓词返回 Bool，不要 `== 1`

```ta
// wrong：pair? 返回 Bool，== 1 是类型错误（历史上曾是恒 false 静默走 else）
if pair?(x) == 1 { ... }     // [E0001] cannot unify Int with Bool

// correct
if pair?(x) { ... }
```

（`is_op` / `is_pipeable_head` 等**自定义**谓词返回 Int 0/1，那才用 `== 1`。）

### car/cdr 不吃 nil

```ta
// wrong：nil 不是 Pair；静默时代是 car(nil) -> nil，现在可证明的 nil 实参编译报错
car(nil)                      // [E0001] cannot unify Nil with Pair

// wrong：取第 k 个元素靠嵌套 cdr，nil 时离根因很远的地方才爆
car(cdr(cdr(x)))

// correct：nil 安全的访问器
list.nth(x, 2)
```

### 字符串拼接

```ta
// wrong：str.concat 恰好 2 参，3 参报 arity 错
str.concat(a, b, c)    // [E0003] wrong number of arguments for 'str.concat' (got 3, want 2)

// wrong：<> 两侧不留空格，被读成标识符 'a<>b'（字面量旁安全，但统一留空格）
a<>b

// wrong：<> 链是 O(n²)
acc = acc <> chunk          // 循环里反复拼

// correct
str.concat(str.concat(a, b), c)
a <> b
buffer.push(acc, chunk)
```

### 模式匹配

```ta
// wrong：拼错变体名——现在编译报错（unknown constructor），历史上是静默死臂
match c { Grn -> 2 ... }    // [E0005] unknown constructor 'Grn' in match on Color

// wrong：定长臂不吸收多余元素，3 元素链表静默走空
match binds { [Bind(k, v), _] -> ... }   // 只匹配长度恰为 2 的列表

// correct：活尾巴吸收剩余
match binds { [Bind(k, v), .. | rest] -> ... }
```

### use

```ta
// wrong：RHS 必须吃延续，42 不是函数——type error，不是运行时崩溃
use x <- 42

// correct
use x <- mpc.then(p)
```

### 管道

```ta
// wrong：右侧只允许调用 / 裸名 / 构造子
5 |> 1 + 2                 // parse error
5 |> { show(7) }           // parse error
a |> b || c                // parse error

// 注意：a || b |> c 解析为 c(a || b)，|| 结果是 Bool，传非 Bool 参数报 E0001
```

### 顶层

```ta
// wrong：顶层只能定义函数、类型、import、const
fn main() { ... }
print("hi")                // parse error：顶层表达式

// correct
fn main() { print("hi") }
```

---

## 12. 规则清单

- [ ] 所有值不可变；没有可变状态、没有数组、没有宏
- [ ] 顶层只能有函数 / 类型 / import / const
- [ ] 整数 48 位，溢出静默回绕；除零/模零杀死当前进程
- [ ] Int 与 Float 严格同型运算，不隐式转换；跨塔走 `int.to_float` / `float.to_int`
- [ ] `==` 同型比较，仅与 nil 的比解放行
- [ ] `&&` / `||` 短路；没有逻辑非 `!`
- [ ] 谓词 `x?` 返回 Bool，`if` 里裸用
- [ ] `nil` 不是 Pair；取元素用 `list.nth`
- [ ] `str.concat` 恰好 2 参；`<>` 两侧留空格；大量拼接用 buffer
- [ ] `|>` 右侧只接调用 / 裸名 / 构造子，最低优先级
- [ ] use 在块首，RHS 必须吃延续
- [ ] match 按臂顺序，首命中生效；ADT/List 穷尽性编译检查，拼错构造子编译报错
- [ ] 类型错误是编译期硬错误（`[E00xx]`）
- [ ] 类型位置大写 = 具体类型，小写 = 类型变量
- [ ] 调用用户函数实参个数必须与声明一致（E0003）
- [ ] 跨模块引用必须带模块前缀（函数、类型、构造子、const）
- [ ] Resource 不得 send 给自己 / 进定时消息 / 走 selective receive
- [ ] actor 崩溃只杀自己；monitor 收 `['DOWN, ref, pid, reason]`

---

## 13. 附录：完整示例

### 基本 actor 系统

```ta
type Msg { Ping(Pid); Pong; Stop }

fn server() {
  match recv() {
    Ping(from) -> {
      send(from, Pong)
      server()
    }
        Pong -> server()                // 兜住其他臂：邮箱是 FIFO，Pong 也可能到
    Stop -> print("done")
  }
}

fn main() {
  let pid = spawn(fn { server() })
  send(pid, Ping(self()))
  match recv() {
    Pong -> print("got-pong")
    _    -> nil                // FIFO：兜住任何先到消息
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

fn supervisor(main_pid) {
  let pid = spawn(fn { worker(0) })
  let ref = monitor(pid)
  send(pid, 'crash)
  sup_loop(main_pid, pid, ref, 0)
}

fn sup_loop(main_pid, pid, ref, count) {
  match recv() {
    ['DOWN, r, p, reason] -> {
      print("worker died")
      if count < 2 {
        let new_pid = spawn(fn { worker(count + 1) })
        let new_ref = monitor(new_pid)
        send(new_pid, 'crash)
        sup_loop(main_pid, new_pid, new_ref, count + 1)
      } else {
        print("giving up")
        send(main_pid, 'supervisor-done)
      }
    }
  }
}

fn main() {
  let self_pid = self()
  spawn(fn { supervisor(self_pid) })
  match recv() {
    'supervisor-done -> print("PASS")
  }
}
```

### 尾递归（TCO）

```ta
fn sum(r, i) {
  if i == 0 {
    r
  } else {
    sum(r + 1, i - 1)    // 尾调用：500 万次迭代不爆栈
  }
}

fn main() {
      print(sum(0, 5000000))
}
```