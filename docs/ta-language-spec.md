# TinyActor (TA) 语言规范

## 1. 第一个程序

```ta
type Msg { Ping(from); Pong; Stop }

fn server() {
  match recv() {
    Ping(from) -> {
      send(from, Pong)
      server()
    }
    Pong       -> server()
    Stop       -> print("server done")
  }
}

fn main() {
  let pid = spawn(fn { server() })
  send(pid, Ping(self()))
  match recv() {
    Pong -> print("got-pong")
    _    -> nil
  }
  send(pid, Stop)
}
```

`tinyactor run hello.ta` 输出 `got-pong`（server 的打印顺序不定，两个进程并发）。

读法：

- `server` 是一个**进程**：`spawn(fn { server() })` 把它放进新进程运行，
  每个进程有独立栈和**邮箱**。`send(pid, m)` 把消息 `m` 投递进 `pid` 的邮箱，
  `recv()` 阻塞取下一条。进程之间**只**通过消息通信。
- `type Msg { … }` 声明代数数据类型（ADT）：`Ping` 带一个字段，`Pong`/`Stop` 是
  零参变体。`match` 按臂顺序解构，第一个命中的臂执行（§5）。
- `main` 是入口进程：程序从 `main()` 开始，它就是一个普通进程（§8）。

语言画像：

- **Erlang 风格的 actor 模型** + ML/Rust 系语法。编译器自身用 TA 编写（自举）。
- **严格求值**，尾调用优化：尾位置的递归调用不增长栈（§10）。
- **Hindley-Milner 类型推导** + 可选注解。类型错误是**编译期硬错误**
  （`[E00xx]`），编译中止——不存在"类型错误运行时才爆"。
- **进程隔离**：运行期错误（除零、类型隔离死亡）杀死**当前进程**，
  其余进程不受影响（§8.4）。
- **所有值不可变**，没有可变状态、没有数组、没有可变引用、没有宏。
  唯一的状态是进程邮箱。**例外：`Resource` 值（buffer/process/tls 等）
  是 C 侧可变资源的句柄**——对它「写」是原地副作用，不是重绑定（§8.5、§11）。

实现侧事实（编译管线、AST、值表示、自举构建）见 [`ta-compilation.md`](ta-compilation.md)，
本文只描述可观察行为。

---

## 2. 程序结构

一个 `.ta` 文件是一个**模块**，顶层只允许四种成员（顺序不限）：

```ta
import "tinyactor:lib/list" as list    // 模块引入（§11）
pub fn f(x) { … }                      // 函数（pub = 跨模块导出）
type Ctor { … }                        // ADT 声明（§5.3）
const LIMIT = 10                       // 常量（一律导出）
```

**不能**有顶层 `let`、顶层表达式、局部函数定义：

```ta
print("hi")     // parse error：顶层表达式
let x = 42      // parse error：顶层 let
```

### 程序入口

- `fn main() { … }` 是入口：无参、不需要 `pub`。它的返回值被忽略，
  exit code 只由**是否崩溃**决定（main 死亡 → exit 1，正常结束 → exit 0）。
- `main` 是普通进程：可以 `self()`、`recv()`、`spawn`。
- 文件里没有 `main` 也能编译（只是没有入口进程被自动启动）。

### 块与函数体

`{ … }` 是**块表达式**：按顺序执行，求值为最后一个表达式的值。
函数体就是块。

```ta
fn f(x) {
  let y = x + 1      // let：绑定，后续表达式可见
  x * y              // 尾表达式 → 函数返回值
}
```

`let` 是顺序绑定：后面的 `let` / 表达式可引用前面的，反向不行。
局部绑定（形参、`let`、`match`/`receive` 绑定的变量）遮蔽同名顶层定义。

### 注释与空白

`//` 到行尾。缩进无语义，但**二元运算符两侧必须留空格**（§3.1）。

---

## 3. 词法

### 3.1 标识符与"必须留空格"

标识符：字母或 `_` 开头，后续可含字母、数字、`.` `+` `-` `<` `>` `=` `!` `?` `*` `/`。

字符集里有运算符字符，所以**二元运算符两侧必须留空格**：

| 写法 | 读成 |
|------|------|
| `a + b` | 加法 |
| `a+b` | 标识符 `a+b`（未定义变量，E0002） |
| `a <> b` | 字符串拼接 |
| `a<>b` | 标识符 `a<>b` |
| `x == y` | 相等 |
| `x==y` | 标识符 |
| `a . b` | 标识符 `a.b`（模块前缀 / 函数名，**不是**属性访问） |

字面量旁安全（`1+2`、`1 <> "s"`）——数字/引号后面没有标识符字符——
但**统一留空格**，不留空格迟早踩坑。

其他词法边界：

| 写法 | 读成 |
|------|------|
| `x - 1` | 减法 |
| `x -1` | 调用 `x(-1)`（`-1` 是字面量实参）——不是 `x - 1` |
| `x- 1` | 标识符 `x-` + `1`（未定义变量 E0002） |
| `let a = 1; let b = 2` | 同一行两个 let，`;` 是合法的语句分隔 |
| `let s = 1 +\n  2` | 多行表达式合法：行尾运算符后换行 = 继续 |

### 3.2 字面量

| 形式 | 类型 | 说明 |
|------|------|------|
| `42` `-3` | Int | 十进制，48 位（§7.1）。负号只属于字面量：`-x` 不合法，写 `0 - x` |
| `3.14` `-0.5` | Float | 必须 `数字.数字`；不支持 `1.`、`.5`、指数记法 |
| `"hello"` | String | 转义 `\n \r \t \\ \" \'`、`\xNN`；未知转义保留原样 |
| `'foo` | Symbol | 单引号前缀符号，用于消息标签、零参 ADT 变体 |
| `true` `false` | Bool | |
| `nil` | Nil | 空值与空列表（§4） |
| `[1, 2, 3]` | List | 列表字面量（§4） |

**没有**一元负号表达式、`!` 逻辑非、`**` 幂。取反写 `0 - x`，
逻辑非用 `bool.not`（`import "tinyactor:lib/bool"`）。

### 3.3 关键字

```
fn let match if else when spawn send import receive pub type external const
nil true false
```

`use` **不是**关键字：仅当后跟 `<-` 时按 use 绑定解析（§5.4），`use(x)` 是普通调用。

### 3.4 print 的输出格式

`print(x)` 打印一个值并换行：

| 值 | 输出 |
|----|------|
| `1` / `"hi"` / `true` / `nil` | `1` / `hi` / `true` / `nil` |
| `'atom` | `atom`（无引号） |
| `[]`（空列表） | `nil`（与裸 `nil` 同形——列表判 `null?`，不看打印） |
| `[1, 2, 3]` | `(1 2 3)`（s-expr 风格，空格分隔） |
| `Point(1, "a")`（本地带参变体） | `(tuple Point 1 a)` |
| `option.Some(42)`（跨模块变体） | `(tuple option.Some 42)`（**带模块前缀**） |
| `1.5` | `1.5` |
| Resource | `<resource>` |

---

## 4. 列表

列表是 `nil` 结尾的 cons 链，**同构**（所有元素同一个类型）：

```ta
fn main() {
  let xs = [1, 2, 3]            // 字面量
  let y  = cons(4, xs)          // 手工构造：4 接在 xs 前
  print(y)                      // (4 1 2 3)
}
```

| 函数 | 类型 | 说明 |
|------|------|------|
| `cons(h, t)` | `a -> List(a) -> List(a)` | 在列表前接一个元素，返回 `List(a)`（推导见 §6.4） |
| `car(lst)` / `cdr(lst)` | `'a -> 'b` | 取头 / 取尾。**`Nil` 不是 cons**：可证明的 nil 实参编译期 E0001（`cannot unify Nil with List('a)`）；运行期才出现的 nil 以 `cartype`/`cdrtype` 隔离死亡 |
| `null?(x)` | `'a -> Bool` | 是不是 nil |
| `cons?(x)` | `'a -> Bool` | 是不是非空列表（cons cell） |

- **取第 k 个元素用 `list.nth`**（`import "tinyactor:lib/list" as list`），
  nil 安全、越界返回 nil。不要写 `car(cdr(cdr(x)))`。
- 遍历先判 `null?`/`cons?`（模式匹配写法见 §5）。
- 列表的构造子形态是 `nil` 与 `cons`，所以 `match [h | t] -> …` 与
  `match cons(h, t) -> …` 等价（§5.2）。

---

## 5. 核心语法

### 5.1 函数

| 形式 | 说明 |
|------|------|
| `fn f(x, y) { body }` | 命名函数（顶层） |
| `pub fn f(x) { … }` | 跨模块导出（§11） |
| `fn f(x: Int) -> Int { … }` | 带类型注解（§9） |
| `fn(x) { … }` / `fn { … }` | 匿名函数（闭包），**只能出现在表达式位置** |

函数是一等值：可绑定、传递、返回。**没有部分应用**：`f(x, y)` 必须一次给齐参数。
**没有局部函数定义**：函数体内不能定义命名函数，匿名函数也无名可自引
——所有递归辅助函数都提到顶层。

```ta
fn apply(f, a, b) { f(a, b) }

fn main() {
  let inc = fn(x) { x + 1 }     // 闭包可捕获局部变量
  print(inc(1))                 // 2
}
```

### 5.2 模式匹配 `match`

```ta
match scrutinee {
  pattern -> expr
  pattern when guard -> expr    // guard 为 false 时跳过该臂
  _ -> default
}
```

- 按臂顺序尝试，**首个**命中且 guard 为真的臂生效
- guard 可引用 pattern 绑定的变量，支持 `&&` / `||`
- 臂之间**换行分隔**，同行多臂可用 `;`（`match r { Ok(v) -> v; Err(_) -> d }`）；
  臂体是 `{ … }` 块或表达式
- 无臂命中时 match **静默求值为 nil**——穷尽性检查（§5.2.2）会拦掉
  ADT/列表的大多数漏臂，但 scrutinee 类型无法推断为具体 ADT/列表时
  （如符号消息的 `recv()`，§8.2）拦不住

#### 模式语法

| 模式 | 匹配 | 示例 |
|------|------|------|
| 整数字面量 | 精确 | `42 ->` |
| 符号字面量 | 精确 | `'hello ->` |
| `nil` / `true` / `false` | 精确 | `nil ->` |
| 小写裸名 | **变量绑定**，匹配任何值 | `n ->` |
| `_` | 通配 | `_ ->` |
| `Name`（大写、零参） | 零参构造子 | `Red ->` |
| `Name(a, b)` | 构造子：标签相等 + 逐参解构 | `Ok(v) ->` |
| `mod.Name(a)` | 跨模块构造子 | `option.Some(v) ->` |
| `cons(a, b)` | 列表 head/tail 解构 | `cons(h, t) ->` |
| `[a, b, c]` | 定长列表（长度恰好 3） | `['DOWN, r, p, reason] ->` |
| `[a, b \| rest]` | 前 2 定长 + 活尾（长度 ≥ 2） | `[h \| t] ->` |

- **大写**构造子模式按标签严格相等；`mod.Name` 看**最后一段**
  （大写 = 构造子，小写 = 变量/调用，`bufio.read(v)` 不构成模式）
- 定长方括号臂**不吸收多余元素**：3 元素列表遇 `[a, b]` 不匹配——
  遍历用 `[h | t]` 或 `cons(h, t)`
- 其余 token（裸算符、`{`、`,`）不构成模式，是 parse error——不会静默降成 `_`
- 列表两种形态：`nil` / `[]`（**同一个值**，`match nil` 与 `match []`
  等价）与 `cons(h, t)` / `[h | t]`（尾必须是活变量/通配）

#### 5.2.2 穷尽性检查

编译器对 **ADT match** 与 **列表 match**（scrutinee 可推断为 `List(E)`）做
穷尽性检查：缺变体且无兜底臂（变量 / `_` / 活尾）时编译拒绝：

```
[E0005] non-exhaustive match: missing Blue
```

- 有意部分匹配要显式写 `_ ->` 臂
- 嵌套构造子只算**部分覆盖**：
  `[E0005] cannot prove exhaustive match: nested patterns cover only part of Some — add an explicit _ -> arm`。
  检查是**充分条件**——真正穷尽但需交叉推理的 match 也会被拒；
  把里层绑成变量逐层 match 是可静态验证的写法
- **未知构造子**是编译错误，不是静默死臂：
  `[E0005] unknown constructor 'Grn' in match on Color`
- **穷尽性检查只在 scrutinee 类型可推断为具体 ADT/列表时触发**。
  `recv()` 返回 `'a`：match 臂的构造子会把 `'a` unify 成那个 ADT，
  **此时照常检查**（`match recv() { Add(1) -> … }` 漏 `Sub` 照样 E0005）；
  但符号消息（`'ping -> …`）不约束 `'a`，类型保持自由，检查无从触发——
  漏臂静默 nil，随后在别处爆掉。写 actor 循环时兜底臂不能省

### 5.3 ADT 声明

```ta
type Color { Red; Green; Blue }
type Option { None; Some(value) }
type Rat { R(num : Int, den : Int) }      // 标签字段（可选）
```

- 变体之间用 `;` 分隔，可换行，末项 `;` 可省
- 零参变体的值就是符号（`Red` → `'Red`）；带参变体的值是**定长扁平 tuple**
  （`Some(42)` 第 0 槽是符号 `'Some`，其余是字段）
- **字段可以带标签**（`R(num : Int, …)`）：调用处可按名传参、顺序任意、
  可用 `..base` 更新（§5.3.1）。不带标签的字段只能按位置传
- **泛型：显式声明类型参数表**。参数**按名**代入字段类型——
  参数 `a` 替换所有字段类型里出现的裸名 `a`：

```ta
type Option(t) { None; Some(value) }
type L(a, b) { L(x : a, y : b) }        // 字段标签可选

// L(1, "s") → L(Int, String)
// type M(a, b) { M(x : b, y : a) } 里 M("s", 1) → M(Int, String)
// （字段 x 的类型是 b=Int，字段 y 的类型是 a=String）
```

- **未注解的字段按位置绑参数**：`Option(t) { Some(value) }` 里 `value`
  的类型就是 `t`（`Option(Int)` 的 `Some(5)` 载荷是 Int）——
  这就是标准库 `Option`/`Result`/`list` 的写法

  隐式小写字段（无参数表）的字段类型**不在变体之间统一**——
  `type M { Ping(from); Pong(to) }` 里 `from`/`to` 各自独立推导，
  使用点推成什么就是什么（Pid、Int 都行）；想要共享类型用显式参数表

- 隐式小写字段（`type R { Ok(v); Err(e) }`，**没有**参数表）也能推导，
  但**该 ADT 不能在注解处应用类型**（`R(Int, String)` 会报 cannot unify）——
  要注解就用显式参数表，要省事就全程裸名（§6.6）
- **递归 ADT** 合法：`type Tree { Leaf; Node(l, v, r) }`
- 跨模块引用：类型名、构造子名都是模块成员，必须带前缀（§9.2）

#### 5.3.1 按标签构造与更新

```ta
type Rat { R(num : Int, den : Int) }

R(3, 4)             // 位置
R(num: 3, den: 4)   // 按名
R(den: 4, num: 3)   // 名字寻址，顺序任意
let half = R(1, 2)
R(..half, num: 5)   // 更新：拷贝 half 全部字段，替换点名的 → R(5, 2)
```

- 一次调用要么**全是位置**、要么**全是标签**，混用是 parse error
- `..base` 只在参数表第一位，且**只求值一次**（`base` 是任意表达式，可嵌套 `R(..R(..r, num: 1), den: 2)`）；`base` 必须同类型（否则 E0001）
- 不带 `..base` 时每个字段都要给：漏 → `[E0007] 'R' is missing field 'den'`
- 写错标签 `[E0007] 'R' has no field 'z'`；重复 `[E0007] duplicate field 'num' in R(...)`
- 零参变体没有字段：`Red(..r)` → `[E0007] 'Red' has no fields to update`
- 标签**不进模式**：`R(num: 3, den: 4)` 的值就是 `R(3, 4)`，解构按位置写

### 5.4 use 绑定（do-notation）

块首的 `use x <- expr` 把块内其余部分作为延续传给 expr：

```ta
use x <- expr
rest...
// 等价于：expr(fn(x) { rest... })
```

- 必须位于**块首**（`let` 之后亦可），可链式；块中间出现是 parse error
- `use <- expr` 丢弃结果
- expr 必须返回**吃延续的函数**：`use x <- 42` 是 type error
  （`cannot unify Int with 'a -> 'b`），不是运行时崩溃

```ta
import "tinyactor:lib/mpc" as mpc

fn parse_two() {
  use k <- mpc.then(mpc.lit("a"))
  use v <- mpc.then(mpc.lit("b"))
  mpc.ret(cons(k, cons(v, nil)))
}
```

- `<-` 由 `<` `-` 两个 token 组成：`use < -x` 与 `use <-x` 无法区分，一律按绑定解析

### 5.5 控制流

```ta
fn main() {
  print(if 2 > 0 { 1 } else { 0 })   // 1
}
```

- `if` 是**表达式**：条件必须 Bool，求值为命中分支的值
- **没有 else 时 else 分支为 nil**：`if done { print("done") }` 等价
  `if done { print("done") } else { nil }`
- 没有 while/for：循环 = 尾递归（§10）

### 5.6 运算符与优先级

| 优先级（高→低） | 运算符 | 说明 |
|------|------|------|
| 1 | `*` `/` `%` | 算术，严格同型（§7）；`%` 仅 Int |
| 2 | `+` `-` `<>` | 算术 + 字符串拼接（`<>` 脱糖为 `str.concat`） |
| 3 | `<` `>` `<=` `>=` `==` `!=` | 比较，严格同型（§7.2）；**右结合** |
| 4 | `&&` | 短路 |
| 5 | `\|\|` | 短路 |
| 6 | `\|>` | 管道，左结合，最松 |

同优先级左结合（`10 - 2 - 3` → `5`）。无括号的函数类型分组见 §9.3。

**管道 `|>`**：右侧只允许**调用 / 裸名 / 构造子**，脱糖为把左侧当作第一个实参：

```ta
x |> f(a, b)    // →  (f x a b)
x |> f          // →  (f x)
x |> Foo(a)     // →  (Foo x a)（构造子）
x |> 1 + 2      // parse error
x |> { show(7) }// parse error
```

`a || b |> c` 解析为 `c(a || b)`。

**注意：stdlib 是"数据在后"风格**（`list.map(f, lst)`），管道把数据放到
**第一个**实参，所以 `lst |> list.map(f)` 是 type error。
管道适合自定义的"数据在前"函数链；对 stdlib 直接用普通调用。

### 5.7 内建数据函数

| 函数 | 类型 | 说明 |
|------|------|------|
| `null?` `cons?` `int?` `string?` `symbol?` `float?` `bool?` `tuple?` | `'a -> Bool` | 类型谓词，返回 **Bool**（`if` 里裸用，§12） |
| `tuple-ref(t, i)` | `'a -> 'b` | 元组第 i 槽（越界杀死当前进程） |
| `tuple-len(t)` | `'a -> 'b` | 槽数 |
| `print(x)` | `'a -> 'b` | 打印（格式见 §3.4） |
| `int.to_float(n)` / `float.to_int(d)` | Int -> Float / Float -> Int | 跨塔转换（§7.3） |
| `INT48_MIN` / `INT48_MAX` | Int | 48 位整数的边界常量 |

### 5.8 字符串（str 模块）

**两层**：

- **低层原语**——编译器内置类型，**无需 import**，任何文件可用：

| 函数 | 类型 | 说明 |
|------|------|------|
| `str.length(s)` | String -> Int | 长度 |
| `str.concat(a, b)` | String -> String -> String | 拼接（**恰好 2 参**，`<>` 的底层） |
| `str.eq(a, b)` | String -> String -> Bool | 相等（`==` 对 String 也合法，§7.2） |
| `str.char_at(s, i)` | String -> Int -> Int | 第 i 字符 ASCII 码（越界 -1） |
| `str.substr(s, i, n)` | String -> Int -> Int -> String | 子串（越界截断） |
| `str.to_int(s)` | String -> Int | 解析（失败 0） |
| `str.from_int(n)` | Int -> String | 转字符串 |
| `str.index_of(s, sub)` | String -> String -> Int | 查找（未找到 -1） |
| `str.to_sym(s)` / `str.sym_to_str(sym)` | String -> Symbol / Symbol -> String | 互转 |

- **高层函数**——`import "tinyactor:lib/str" as str` 后可用：
  `str.split(s, sep)`、`str.join(lst, sep)`、`str.trim(s)`、`str.replace(s, o, n)`、
  `str.upper(s)`、`str.lower(s)`、`str.parse_float(s)`、`str.from_float(f)`、
  `str.from_base(s, base)`、`str.to_base(n, base)`、`str.cmp(a, b)`

  **别名规则**：`import "…:lib/str" as X` 之后，本文件里裸名 `str`
  就被遮蔽——`X` 不是 `str` 时低层 `str.length` 等**全部不可用**
  （报错 `the real name is not usable here`）。要两套都写，别名就取
  `str`（两层合并在同一前缀下，`str.length` 和 `str.split` 都能用）

  这些高层函数返回类型是 `'a`（无约束）——**类型检查器拦不住实参形状错误**：
  `str.join([1, 2], "-")`（Int 列表）静默输出空串，不报错。
  拼 Int 列表先 `list.map(fn(x) { str.from_int(x) }, …)`。

**大量拼接用 `buffer`**（`import "tinyactor:lib/buffer" as buffer`），
不要写 `<>` 链（O(n²)）：

```ta
import "tinyactor:lib/buffer" as buffer
import "tinyactor:lib/result" as result

fn join_all(parts) {
  let b = result.unwrap(buffer.new())
  // …循环里 buffer.push_str(b, chunk)（返回 Result）…
  result.unwrap(buffer.to_string(b))
}
```

buffer 的操作都返回 `Result`（`push_str` 成功为 `Ok(长度)`）；
`new()` 不会失败但形状仍是 `Result`——一律 `result.unwrap`（§11）。

---

## 6. 类型系统

### 6.1 推导与注解

Hindley-Milner 推导：**未注解函数照常推导**，注解是可选的精确化。
类型错误是编译期硬错误，格式：

```
[E0001] in function 'f' (line N): cannot unify Int with Bool
```

```ta
fn apply(f : fn(Int, Int) -> Int, a : Int, b : Int) -> Int { f(a, b) }
fn pick(flag : Bool) -> fn(Int, Int) -> Int {
  if flag { fn(x, y) { x + y } } else { fn(x, y) { x * y } }
}
```

- **命名规则（无例外）**：类型位置首字母**大写** = 具体类型（内建或 ADT），
  首字母**小写** = 类型变量（隐式多态）
- 内建具体类型：`Int Float String Bool Symbol Pid List Nil Resource Atom`
  与用户 ADT（跨模块写 `mod.Name`）
- 类型变量两种等价拼法：`fn f(x : a) -> a` 与 `fn f(x : 'a) -> 'a`
  （`'a` 是 parser 接受的记号，语义相同；标准库用裸名 `a`）
- 复合类型：`List(Int)`、`Result(Int, String)`、`Option(a)`

### 6.2 函数类型

`a -> b` 与 `fn(A, B) -> R` 是**同一个类型**的两种拼写（后者脱糖为 curried arrow）：

| 写法 | 结果 |
|------|------|
| `fn(A, B) -> R` | 合法 |
| `fn -> Int` | 错误：`'fn' in a type needs a parameter list` |
| `fn()` | 错误：`fn() with no parameters is not a type` |
| `fn(A, B)` | 错误：`function type needs a result` |
| `(Int -> Int) -> Int` | 不支持括号分组，写 `fn(Int -> Int) -> Int` |

### 6.3 调用 arity

- 直接调用用户函数：实参个数必须与声明一致，否则
  `[E0003] wrong number of arguments for 'f' (got N, want M)`
- 函数**作为值**传递、内部以别的个数调用时 typecheck 看不到；
  VM 侧响亮 fatal（`arity mismatch`），不会静默读错位槽

### 6.4 推导边界

- **`cons` 只建同构列表**：签名 `a -> List(a) -> List(a)`，
  `cons(1, nil)` 直接推出 `List(Int)`，与字面量 `[1]` 同型；
  `cons(1, "x")` 是类型错误（`arg 2 of cons: cannot unify String with List(Int)`）。
  异构「记录」用声明的 record / ADT，不要拿 cons 拼点对
- **nil 推导为自由变量**：既是空值又是空列表，HM 无法区分——所以
  `fn f(lst : List(Int)) { match lst { [] -> … } }` 合法（`[]` 模式
  unify 进 `List(Int)`）；错误信息里打印成 `Nil`（如 `car(nil)` 的
  `cannot unify Nil with List('a)`）只是展示名，不是说 nil 有具体类型 `Nil`
- **没有联合类型**："符号或表"这种形状只能写 `'a`

### 6.5 两个逃生舱

- **`Atom`**：唯一的"我不知道类型"标注。与任何类型 unify 成功且**不绑定**
  任何一边——返回 `Atom` 的函数可以在各分支返回不同形状，
  拿回来的值仍按真类型参与运算（编译期放手，不是运行期装箱）。
  只在"一个位置本来装多种形状、HM 没有 union 可写"时用；
  别处出现基本都是"类型没想清楚"的信号
- **底类型（`Never`）**：`1 / 0`、`x % 0`（Int **字面量**零除数）永远不产生值，
  其类型与任何类型 unify 且不约束对面。fail-fast 惯用式因此能过 typecheck：

```ta
fn unwrap(r) {
  match r {
    Ok(v)  -> v
    Err(_) -> 1 / 0    // 崩溃臂不参与臂类型推断 → unwrap : R(a, b) -> a
  }
}
```

  `1.0 / 0.0` 是 IEEE inf（不死）；`x / y`（变量除数）是普通 Int 运算。

### 6.6 泛型 ADT 的注解

注解能否应用类型，取决于 ADT 怎么声明的：

| 声明 | 注解写法 |
|------|------|
| `type Option(t) { None; Some(value) }`（显式参数表） | `Option(Int)`、`option.Option(String)`（跨模块）——**惯用法** |
| `type R { Ok(v); Err(e) }`（隐式参数） | 只能裸 `R`，或干脆不注解；`R(Int, String)` 是 type error |

```ta
import "tinyactor:lib/option" as option

fn f(o : option.Option(Int)) -> Int {          // 显式参数表 → 可应用类型
  match o {
    option.Some(v) -> v
    option.None    -> 0
  }
}

type R { Ok(v); Err(e) }                        // 隐式参数
fn unwrap_or(d, r) {                            // 全程裸名推导，同样正确
  match r { Ok(v) -> v; Err(_) -> d }
}
```

- 具体类型代入（`Option(Int)`）合法且是库的惯用写法；
  类型变量代入（`fn f(o : Option(a)) -> a`）**也合法**——
  库签名就这么写（`option.values : List(Option(a)) -> List(a)`）
- 隐式 ADT 的值推导为裸类型（`R`），靠 match 逐变体收窄；
  这正是 §6.4 "没有联合类型" 与逃生舱的日常用法

---

## 7. 算术语义

### 7.1 Int：int48 回绕

整数是 **48 位二进制补码**，范围 **[-140737488355328, 140737488355327]**
（即 `INT48_MIN` … `INT48_MAX`，两个都是可引用常量）。

- `+` `-` `*` 溢出**静默按模 2^48 回绕**：不报错、不饱和、不升级浮点
  - `INT48_MAX + 1` → `INT48_MIN`
  - `140737488355328 == -140737488355328` 为 true（同一归一化）
- `/` `%` 遵循 **C 语义**：商向零截断，余数符号跟随被除数
  - `-7 / 2` → `-3`；`-7 % 2` → `-1`（不是 Python 的 1）
- `INT48_MIN / -1` 回绕为 `INT48_MIN`；`INT48_MIN % -1` → `0`

**除零 / 模零**（`x / 0`、`x % 0`）杀死**当前进程**（reason `'divzero`）：

```
** CRASH pid 0: 'divzero
   at main (fn 4)
```

- monitor 方收到 `['DOWN, ref, pid, 'divzero]`
- main 进程死亡 → 程序 exit 1；子进程死亡 → 其余进程继续，exit 不受影响
- 浮点除零不死亡（`1.0 / 0.0` → inf）

### 7.2 相等与比较

`==` `!=` `<` `>` `<=` `>=` 要求操作数**同型**：
Int~Float、Bool~Int、String~Int 跨类型比较一律 type error。

- String `==` String 合法（`"a" == "a"` → true）；`str.eq` 是等价替代
- **与 `nil` 的比较永远合法**（`x == nil`、`nil != false`）：
  nil 是单例，不约束另一侧类型
- `<` 等只接受 Int 或 Float 同型对；字符串排序用 `str.cmp`

### 7.3 严格数值塔

Int 和 Float 是完全不同的类型，任何运算符都不做隐式转换：

```ta
1 + 2         // ok
1.5 + 2.0     // ok
3 + 0.5       // type error: cannot unify
2.5 < 3       // type error
```

- `%` 仅 Int
- Float 是 IEEE 754 double：`1.0 / 0.0` → inf，`0.0 / 0.0` → nan；
  `nan != nan` 成立、`nan == nan` 不成立
- 跨塔转换（唯一合法途径，**内建，无需 import**）：

| 函数 | 类型 | 语义 |
|------|------|------|
| `int.to_float(n)` | Int -> Float | 精确升宽 |
| `float.to_int(d)` | Float -> Int | 向零截断；越界按 int48 回绕；NaN → 0 |

---

## 8. Actor 模型

### 8.1 原语

| 操作 | 类型 | 说明 |
|------|------|------|
| `spawn(f)` | `Pid` 返回 | 新进程运行 `f`，返回 Pid。`f` 必须是**零参闭包**——直接传顶层函数名（`worker`，§8.1）或闭包（`fn { server(cfg) }`，可捕获初值）。传其它任何值（含符号 `'worker`、函数**调用结果** `worker("a")`——TA 没有部分应用）运行期死 `spawn on non-closure`。**注解里写不出「零参函数」**：`fn -> Pid`、`fn() -> Pid` 都是语法错误（§6.2），也没有 `forall` 语法——内部类型 `forall(a, a -> Pid)` 对任何值通过检查，所以高阶参数注解只写具体箭头形（如 `Int -> Pid`）或不写 |
| `send(pid, msg)` | `Pid -> 'a -> 'b` | 异步投递任何值 |
| `recv()` | `'a` | 阻塞，FIFO 取下一条（§8.2：穷尽性检查规则见 §5.2.2） |
| `recv_after(ms)` | `Int -> 'a` | 至多等 ms 毫秒，超时返回 nil，邮箱不动 |
| `receive { … }` | 见 §8.2 | 选择性接收 |
| `self()` | `Pid` | 自身 Pid |
| `monitor(pid)` | `Pid -> Int` | 返回 Int ref；pid 死亡时收 `['DOWN, ref, pid, reason]` |

```ta
fn worker() { print("worker running") }
fn server(config) { print(config) }

fn main() {
  spawn(worker)                      // 直接传无参顶层函数名
  let config = "cfg"
  spawn(fn { server(config) })       // 闭包捕获初值——传参只能靠闭包
    recv_after(100)                     // 短暂等待，让子进程有机会跑
}
```

**spawn 永远调用 0 参函数**：`spawn(fn(x) { loop(x) })` 运行期 arity fatal。
想传初值就闭包捕获：`spawn(fn { loop(0) })`。

### 8.2 recv 与 receive

```ta
fn handler() {
  // recv：严格 FIFO。符号消息不约束类型 → 漏臂无检查、静默 nil（§5.2.2）
  match recv() {
    'ping -> print("ping")
    'pong -> print("pong")
    _     -> 1 / 0                  // 兜不住就响亮死（reason 'divzero）
  }
  // receive：选择性接收，扫描邮箱，不匹配的**保留**
  receive {
    'ping -> "got ping"
    'pong -> "got pong"
  }
}
```

- `receive` 无匹配时**阻塞等新消息**（Erlang 语义），不是立即返回 nil
- `receive` 的臂与 match 同语法，不做穷尽性检查
- `recv_after`：**消息优先**——deadline 前有消息则返回消息，恰好同时消息赢；
  超时不消费；`0` = 只探测当前邮箱；精度受调度粒度限制（约 1–100ms 额外延迟）

### 8.3 进程生命周期与 monitor

- 进程死亡原因：运行期错误（`'divzero`、`cartype`…）或正常返回
- **正常返回也发 DOWN**：`reason` 为 `nil`；崩溃时 reason 是错误符号
- 崩溃时 stderr 打印 `** CRASH pid N: 'reason` 及函数位置
- main 死亡 → 程序结束（exit 1）；非 main 死亡 → 其余进程继续运行

### 8.4 进程隔离

运行期错误只杀当前进程。这是语言的核心承诺：

```ta
fn worker() {
  recv()
  1 / 0                        // worker 死（'divzero），main 活着
}

fn main() {
  let pid = spawn(fn { worker() })
  let ref = monitor(pid)
  send(pid, 'go)
  match recv() {
    ['DOWN, r, p, reason] -> print("worker died")
  }
}
```

### 8.5 Resource 值的传递限制

`Resource` 是 C 模块的不透明资源值：无构造子、不可 match、不可比较
（按 header 指针恒等）、打印 `<resource>`。`send`/`spawn` 时**移交归属**
（接收方成为 owner），因此三条硬限制：

1. **不得 send 给自己**（自己移交是空操作，旧副本会被 GC 释放）
2. **不得进定时消息**（`timer.send_after` / `send_interval` 的消息体）
3. **不得走 selective receive**（`receive { … }`）

定时器（`import "tinyactor:lib/timer" as timer`）：

| 函数 | 类型 | 说明 |
|------|------|------|
| `timer.send_after(pid, ms, msg)` | `Pid -> Int -> 'a -> Int` | ~ms 后投递 msg，返回 timer id |
| `timer.send_interval(pid, ms, msg)` | `Pid -> Int -> 'a -> Int` | 每 ~ms 投递，返回 id |
| `timer.cancel(id)` | `Int -> Int` | 1 = 取消成功，0 = 已触发/无效 |

---

## 9. 模块系统

### 9.1 import

```ta
import "example.org/hello:lib/greet"              // 唯一形态：<树名>:<树内路径>
import "example.org/consumer:app/screen" as screen // 显式别名（可省：默认 = 末段）
import "tinyactor:lib/json" as j
```

- **名字只有一种写法**：`<树名>:<树内路径>`，省略 `.ta`。没有裸名、相对形式、
  `std:`——没有第二种拼写、没有候选列表
- 解析是一次机械拼接：`<根>/vendor/<树名>/<树内路径>.ta`。根 = 入口源文件
  向上第一个含 `vendor/` 的祖先目录。`tinyactor init` 在根目录建两样东西：
    `vendor/tinyactor -> <安装根>` 符号链接（工具链标准库树）+
  `tinyactor.deps`（树清单）；第三方树照此加链接即可，没有别的隐藏文件
- **手建 `vendor/<树名>/` 目录就能 import**——不需要 `tinyactor.deps`
  登记、不需要符号链接（deps 只是 `tinyactor init` 的记录文件）
- 失败只有三种（响亮拒绝）：

```
error: invalid module name: <name>           // 名字违规
error: module not found: <name>              // 拼出的路径读不到，附 tried / root
error: no vendor/ directory above <src>      // 有 import 却没有根
```

- 卫生约束：名字必须含且只含一个 `:`；段全非空、禁 `.` / `..` 点段
- 一个文件 = 一个单元，菱形依赖只加载一次；环显式报错
  `error: circular import: a -> b -> a`
- 语言不知道"版本"：冲突 / fetch / lock 是用户与工具的事
- 完整契约见 [`modules.md`](modules.md)

### 9.2 pub 导出

```ta
pub fn tokenize(src) { … }     // 跨模块可见：mod.tokenize
fn parse_expr(toks) { … }      // 仅模块内部
pub type Msg { Ping(Pid); Pong } // mod.Msg / mod.Ping
const LIMIT = 10               // const 一律导出：mod.LIMIT（没有 pub const）
```

- 成员非 pub → `[E0004] 'x' is not exported by module 'm'`
- 成员不存在 → `[E0004] module 'm' has no export 'x'`
- 两个模块导出同名函数互不遮蔽（都须带前缀）
- **跨模块引用必须带前缀**（函数、类型、构造子、const），没有 prelude：

```ta
import "tinyactor:lib/option" as option

fn unwrap_or(o : option.Option(Int), d) -> Int {   // 显式参数表 → 应用具体类型
  match o {
    option.Some(v) -> v
    option.None    -> d
  }
}
```

- 别名只作用于声明文件；`tok.tokenize("hi")` 是唯一合法写法，
  用 real name `tokenizer.tokenize("hi")` 编译期报错（real name 被别名占用）
- 裸名查不到绑定是编译错误，附建议：
  `[E0004] 'Some' is not in scope; did you mean 'option.Some'?`

### 9.3 external fn（C 模块声明）

C 模块（`lib/<mod>.dylib`）的类型写在模块自己的声明文件 `lib/<mod>.ta`：

```ta
external fn demo.double(Int) -> Int               // 位置形式
external fn demo.list(a : Int, b : Int) -> List(Int)   // 命名形式（参数名只是文档）
```

- 两种形式语义相同：参数名只给读者看，类型才是检查依据
- 参数位置上的**裸名**（不带 `:`）是无约束类型变量：
  `external fn buffer.raw_push_str(h, s) -> Int` 里 h、s 都是任意类型
- 省略返回类型 = 返回 nil
- 声明是**强约束**：模块被 import 后实参逐个按声明类型合一，不符报 E0001
- **可选参数写成两个名字**：`net.connect`（2 参）与 `net.connect_timeout`（3 参）
  是同一实现的两个固定元数注册项

---

## 10. 求值规则

- **严格求值**：函数实参在调用前求值
- **尾调用优化**：函数体内尾位置（`if` 分支体、块尾表达式、match 臂体）的调用
  不增长栈——500 万次尾递归迭代可用：

```ta
fn sum(r, i) {
  if i == 0 { r } else { sum(r + 1, i - 1) }
}
fn main() { print(sum(0, 5000000)) }
```

- **作用域**：词法作用域；函数可捕获局部变量形成闭包
- **表达式皆值**：`if` / 块 / `match` 都是表达式，可出现在任何表达式位置

---

## 11. 标准库速查

标准库是普通包（`vendor/tinyactor/lib/`），`import "tinyactor:lib/<name>"` 引入。
常用函数签名：

### list（`import "tinyactor:lib/list" as list`）

| 函数 | 类型 |
|------|------|
| `list.length(lst)` | `List(a) -> Int` |
| `list.nth(lst, n)` | `a -> Int -> b`（越界 nil） |
| `list.map(f, lst)` | `fn(a) -> b, List(a) -> List(b)` |
| `list.filter(f, lst)` | `fn(a) -> Bool, List(a) -> List(a)` |
| `list.foldl(f, acc, lst)` | `fn(a, a) -> a, a, List(a) -> a` |
| `list.foldr(f, acc, lst)` | 同上（右折叠） |
| `list.reverse(lst)` | `List(a) -> List(a)` |
| `list.append(a, b)` / `list.concat(lsts)` | 拼接 |
| `list.take(n, lst)` | `Int, List(a) -> List(a)` | 取前 n 个 |
| `list.member(x, lst)` | `a, List(a) -> Bool` |
| `list.zip(a, b)` | `List(a), List(b) -> List(Zip)`（`type Zip { Zip(left, right) }`） |
| `list.range(start, end)` | `Int, Int -> List(Int)`（不含 end） |
| `list.sort(cmp, lst)` | `fn(a, a) -> Int, List(a) -> List(a)`（cmp：负/0/正） |
| `list.chunk(n, lst)` | `Int, List(a) -> List(List(a))` |

### option / result

```ta
import "tinyactor:lib/option" as option

fn main() {
  let o = option.Some(42)
  print(option.unwrap_or(0, o))              // 42
  print(option.map(fn(x) { x + 1 }, o))      // (tuple option.Some 43)
  print(option.is_some(o))                   // true
}
```

option 的 ADT 定义就在 option 模块里（`None` / `Some(value)`，
显式参数表 → 可写 `option.Option(Int)` 注解）。
其他函数：`option.is_none(o)`、`option.values(lst)`
（`List(Option(a)) -> List(a)`）。

`result` 同构：`pub type Result(t, e) { Ok(value); Err(err) }`、
`result.unwrap_or(default, r)`、`result.map(f, r)`、`result.map_err(f, r)`、
`result.bind(f, r)`、`result.is_ok(r)` / `result.is_err(r)`、`result.try(f, r)`。

### dict

```ta
import "tinyactor:lib/dict" as dict

fn main() {
  let d  = dict.int_new()              // Int 键
  let d2 = dict.insert(d, 7, "seven")
  print(dict.get(d2, 7))               // (tuple option.Some seven)
  print(dict.get(d2, 99))              // option.None
  print(dict.has_key(d2, 7))           // true
}
```

`dict.get` 返回 **Option**，不返回裸值——没有"key 不存在返回 nil"的捷径。

### 其他常用模块

| 模块 | 职责 | 代表函数 |
|------|------|------|
| `bool` | 逻辑 | `bool.not(x)` |
| `buffer` | 可变字节缓冲（大量拼接） | `buffer.new()` / `buffer.push_str(b, s)` / `buffer.to_string(b)`（均返回 Result） |
| `strconv` | 进制转换 | `strconv.int_to_str(n, base)` / `strconv.str_to_int(s, base)` |
| `math` | 浮点数学 | |
| `msg` | 消息类型辅助 | |
| `fs` | 文件系统 | |
| `file` | 文件读写 | |
| `timer` | 定时器（§8.5） | |
| `mpc` | parser combinator（use 的主要消费者） | |
| `supervisor` / `gen_server` | actor 行为模板 | |
| `json` / `csv` / `html` / `url` | 编码 | |

---

## 12. 常见错误

每条都是 wrong/correct 成对。

### 谓词返回 Bool，不要 `== 1`

```ta
// wrong：cons? 返回 Bool，== 1 是类型错误（历史上曾是恒 false 静默走 else）
if cons?(x) == 1 { … }     // [E0001] cannot unify Int with Bool

// correct
if cons?(x) { … }
```

（`is_op` / `is_pipeable_head` 等**自定义**谓词返回 Int 0/1，那才用 `== 1`。）

### car/cdr 不吃 nil

```ta
// wrong：nil 不是 cons；静默时代是 car(nil) -> nil，现在可证明的 nil 实参编译报错
car(nil)                      // [E0001] cannot unify Nil with List('a)

// wrong：取第 k 个元素靠嵌套 cdr，nil 时离根因很远的地方才爆
car(cdr(cdr(x)))

// correct：nil 安全的访问器
list.nth(x, 2)
```

### 字符串拼接

```ta
// wrong：str.concat 恰好 2 参，3 参 arity 错
str.concat(a, b, c)    // [E0003] wrong number of arguments for 'str.concat' (got 3, want 2)

// wrong：<> 两侧不留空格，被读成标识符 a<>b
a<>b

// wrong：循环里 <> 链反复拼是 O(n²)
let acc = …; acc = acc <> chunk    // TA 没有 = 赋值：每步都让 acc 变成新 let

// correct
a <> b
str.concat(str.concat(a, b), c)
buffer.push_str(b, chunk)         // 循环拼接用 buffer（返回 Result）
```

### 模式匹配

```ta
// wrong：拼错变体名——编译错误（unknown constructor），历史上是静默死臂
match c { Grn -> 2 }    // [E0005] unknown constructor 'Grn' in match on Color

// wrong：定长臂不吸收多余元素，3 元素列表静默走下一个臂
match binds { [Bind(k, v), _] -> … }   // 只匹配长度恰为 2

// correct：活尾吸收剩余
match binds { [Bind(k, v) | rest] -> … }
```

### use

```ta
// wrong：RHS 必须吃延续，42 不是函数——type error，不是运行时崩溃
fn bad() {
  use x <- 42
}
```

```ta
// correct：use 在块首，RHS = mpc.then(p) 吃延续
import "tinyactor:lib/mpc" as mpc

fn parse_one(p) {
  use x <- mpc.then(p)
  mpc.ret(x)
}
```

### 管道

```ta
// wrong：右侧只允许调用 / 裸名 / 构造子
5 |> 1 + 2                 // parse error
5 |> { show(7) }           // parse error
a |> b || c                // parse error

// 注意：a || b |> c 解析为 c(a || b)——|| 结果是 Bool，
// 传非 Bool 参数报 E0001
```

### 顶层

```ta
// wrong：顶层只能有函数 / 类型 / import / const
print("hi")                // parse error：顶层表达式
let x = 42                 // parse error：顶层 let

// correct
fn main() { print("hi") }
```

### actor 循环漏兜底臂

```ta
// wrong：符号消息不约束 scrutinee 类型，漏臂无检查，静默 nil 后在别处爆
match recv() {
  'ping -> …
}

// correct：自己兜底
match recv() {
  'ping -> print("ping")
  _     -> 1 / 0
}
```

---

## 13. 规则清单

- [ ] 所有值不可变；没有可变状态、没有数组、没有宏
- [ ] 顶层只能有函数 / 类型 / import / const；`fn main()` 是入口
- [ ] 二元运算符两侧留空格；没有 `-x`、`!x`、`x**y`
- [ ] 整数 48 位，溢出静默回绕；除零/模零杀死当前进程
- [ ] Int 与 Float 严格同型运算，跨塔走 `int.to_float` / `float.to_int`
- [ ] `==` 同型比较，仅与 nil 的比解放行
- [ ] `&&` / `||` 短路；没有 `!`，逻辑非用 `bool.not`
- [ ] 谓词 `x?` 返回 Bool，`if` 里裸用
- [ ] `nil` 不是 cons；取元素用 `list.nth`
- [ ] `str.concat` 恰好 2 参；`<>` 两侧留空格；循环拼接用 buffer
- [ ] 优先级：`* / %` > `+ - <>` > 比较（右结合）> `&&` > `||` > `|>`
- [ ] `|>` 右侧只接调用 / 裸名 / 构造子，最低优先级
- [ ] use 在块首，RHS 必须吃延续
- [ ] match 按臂顺序首命中生效；ADT/列表穷尽性编译检查；
      `recv()` 无检查，actor 循环自己兜底
- [ ] 类型位置大写 = 具体类型，小写 = 类型变量
- [ ] 用户函数实参个数必须与声明一致（E0003）
- [ ] 跨模块引用必须带模块前缀（函数、类型、构造子、const）
- [ ] spawn 调用 0 参函数；传初值用闭包捕获
- [ ] monitor 对正常结束也发 DOWN（reason nil）
- [ ] Resource 不得 send 给自己 / 进定时消息 / 走 selective receive

---

## 14. 完整示例

### 基本 actor 系统

见 §1 第一个程序。

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
    _ -> sup_loop(main_pid, pid, ref, count)   // 兜住其他消息
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

### 递归列表处理

```ta
fn sum_lst(lst) {
  match lst {
    []        -> 0
    [h | t]   -> h + sum_lst(t)
  }
}

fn map2(f, lst) {
  match lst {
    []      -> []
    [h | t] -> cons(f(h), map2(f, t))
  }
}

fn main() {
  print(sum_lst([1, 2, 3, 4]))          // 10
  print(map2(fn(x) { x * 2 }, [1, 2]))  // (2 4)
}
```

### 尾递归（TCO）

见 §10（`sum(0, 5000000)` 不爆栈）。`sum(0, 5000000)` 不爆栈）。`sum(0, 5000000)` 不爆栈）。