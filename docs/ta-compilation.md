# TinyActor (TA) 编译与内部实现

本文是 [ta-language-spec.md](ta-language-spec.md) 的配套文档：语言规范只描述
可观察行为，本文记录**实现侧**的事实——编译管线、脱糖映射、AST 形状、值表示、
类型检查器内部、模块内联机制、自举流程。改编译器时以本文为准。

## 1. 编译管线

```
source
 → tokenizer.tokenize          （lib/bootstrap/tokenizer.ta）
 → parser.parse                （lib/bootstrap/parser.ta → form 树 AST）
 → typecheck.infer_program     （lib/bootstrap/typecheck.ta，硬错误）
 → lower-ast.lower_program     （lib/bootstrap/lower-ast.ta → kl 的 Top）
 → compile.compile             （lib/bootstrap/compile.ta → 字节码指令）
 → VM（C，src/vm.c）
```

- 驱动：`lib/bootstrap/driver.ta`（`tinyactor build` / `fmt` 分发、
  resolve_imports / typecheck_cached）
- kl 类型化 AST 与 form ↔ Top 桥：`lib/bootstrap/ast.ta`；
  字节码落盘 `write_bc` + lisp 库文本 `lib_src`：`lib/bootstrap/bytecode.ta`
- `.kl` 文本快照（`lib/bootstrap/driver.kl`）是**本地产物，不入 git**，
  仅供 G-C 测试与观察；格式见 [kl-format.md](kl-format.md)
- 自举种子 `lib/bootstrap.tabc` 是编译产物，**入库**：改任何 `.ta` 源码后
  `make bootstrap` 重新生成并提交（fixed point = 连跑两遍 byte-identical）

## 2. 语法糖 → 编译产物映射

所有脱糖都发生在 parser 或 lower 期，typecheck / codegen / VM 对糖无感知：

| 源码 | 编译产物 | 层 |
|------|----------|----|
| `a <> b` | `(str.concat a b)` | parser |
| `a \|> f(b)` | `(f a b)`（注首参重写为普通调用）；构造子 `a \|> Foo(b)` → `(tuple (quote Foo) a b)` | parser |
| `use x <- e; rest` | `(e (fn (x) rest))` | parser |
| `fn(A, B) -> R` 类型 | `('arrow A ('arrow B R))` | parser |
| `f: fn(Int) -> Int` 等注解 | 额外 `(type-sig name (param_types...) ret_type)` 表单；codegen 跳过 | parser |
| `[a, b, c]` | `(list a b c)` 特殊形式 → codegen 展开回 `cons` 链 | parser → codegen |
| `match` 表达式 | lower 的 `match_desugar` → 嵌套 `if` + `=` 比较（`.kl` 词表里没有 match） | lower |
| 列表模式 `['DOWN, r, p, reason]` | 链式 `cons` 解构 | lower |
| 构造子调用 `Point(1, 2)` | `(tuple (quote Point) 1 2)` | parser |
| 构造子模式 `Point(x, y)` | `(tuple (quote Point) x y)` | parser |
| `R(num: 3, den: 4)` / `R(..h, num: 5)` | `('ctor.labels (quote R) nil ((num 3) (den 4)))` / `('ctor.labels (quote R) h ((num 5)))` | parser |

match 脱糖示例（lower 生成）：

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

### 按名字构造的 driver 展开

`'ctor.labels` 是编译器私有伪名（与 `'cov.line` 同一层次：不是关键字，
调用头要么是裸标识符要么是 `mod$fn`，产不出它）。参数按**名字**键，重排发生在
driver 展开期——那时 label→槽位表已知（typecheck 按同一张表检查类型）：

```ta
R(num: 3, den: 4)   → (tuple (quote R) 3 4)
R(den: 4, num: 3)   → (tuple (quote R) 3 4)
R(..h, num: 5)      → (let _r h (tuple (quote R) 5 (tuple-ref _r 1)))
```

纯位置调用不经过 `ctor.labels`，发射 `(tuple (quote R) 3 4)`——与标签出现前
逐字节相同。

## 3. ADT 的 AST

```ta
type Color { Red; Green; Blue }
→ (type Color (quote Red) (quote Green) (quote Blue))

type Option { None; Some(value) }
→ (type Option (quote None) (Some (nil value)))

type Rational { R(num : Int, den : Int) }
→ (type Rational nil (R (num (quote Int)) (den (quote Int))))
```

每个字段是 2-list `(label type)`：没写标签的 label 是 `nil`
（`MkPair(a, b)` → `(MkPair (nil a) (nil b))`）。按名字构造查的就是这张
label→槽位表。

注解示例（type-sig 与 define 并列）：

```ta
fn add(x: Int, y: Int) -> Int { x + y }
→ ((type-sig add (Int Int) Int) (define (add x y) (+ x y)))

fn add(x, y) { x + y }
→ ((define (add x y) (+ x y)))
```

## 4. 值表示（VM 侧）

**NaN-boxing**：64 位值中 normal double 原样存储（高 16 位在 tag 区间之外）；
非 double 类型用高 16 位作 tag：

| tag | 类型 | tag | 类型 |
|-----|------|-----|------|
| `0xFFF1` | int (int48) | `0xFFF8` | closure |
| `0xFFF2` | nil | `0xFFF9` | string |
| `0xFFF3` | true | `0xFFFA` | bytes |
| `0xFFF4` | false | `0xFFFB` | closure-id |
| `0xFFF5` | sym | `0xFFFC` | resource |
| `0xFFF6` | pair | `0xFFFD` | tuple |
| `0xFFF7` | pid | | |

tag 区间落在 IEEE 754 负 NaN 区（指数全 1、符号位 1），任何有限 double
（高 16 位 ≤ 0xFFEF）与 -Inf（0xFFF0…）都撞不上；NaN 位模式装箱时统一规范化
为固定 quiet NaN。

**Int**：48 位载荷（`val_payload48`），读取时从第 47 位符号扩展
（`src/val.c` 的 `val_int` / `val_get_int`）。任何 Int 值装箱时归一化到
[-2^47, 2^47-1]（`src/num.c`）。算术在 int64 中间结果上进行，`val_int` 装箱
截断低 48 位。`*` 可能溢出 int64（C UB），当前依赖 `-O2` 下的补码回绕行为；
若某编译器/优化组合上出现偏差，正确做法是 Makefile 加 `-fwrapv`。

**字面量路径**：整数字面量在 tokenizer 调 `str.to_int`（`lib/str.c`）时即经
`val_int` 归一化，编译期只把归一化后的值内嵌进 `IConstI` → `OP_CONST_I`，
与运行时算术同一套回绕语义。Float 字面量文本端到端传递，VM 用 `strtod` 解析
（AST 节点 `('float "1.5")`）。

**Tuple**：带参构造子的值是 VM 的第二种 heap kind（`HEAP_TUPLE`，与 cons pair
并列）：第 0 槽是构造子名的符号值（`'Some`，不是嵌套 quote 列表），其余是字段。
定长扁平，`tuple-ref` O(1)。零参变体是符号值（`Red` → `'Red`）。

## 5. 类型检查器内部

- **env 形状**（`extend_global`）：`[局部绑定...] [arity 表] [全局 define...] [内建...]`。
  局部同名即遮蔽，遮蔽后调用不查 arity（否则「形参也叫 `f`」的高阶函数全误报）。
- **arity 检查**：直接调用用户 define 的实参个数取自 define 参数表（不是数类型
  箭头），不符报 E0003；`builtin_arity` 表（「符号 → arity」，-1 = 变参不查）管
  C 原语：`net.listen` 1、`net.write` 2、`net.connect` 2、`net.connect_timeout` 3、
  `net.read` -1……须与 `lib/net.c` 的 `TaFunc` 注册一致（负例
  `test/compiler/net-connect-arity-errors.ta`）。函数当值传递的动态路径由 VM
  `CALL` 的 fn_args 检查响亮 fatal。
- **内建签名表**：`+ - * /`（`Int -> Int -> Int`）、比较（`-> Bool`）、`==`
  （`forall a. a -> a -> Bool`）、`car/cdr/cons`、谓词族（`forall a. a -> Bool`）、
  `tuple-ref/tuple-len`、`spawn/self/send/recv` 等。`not` **不在**承诺表——
  它是 `lib/bool.ta` 的普通函数，签名来自其注解（分层原则：能在库层解决的不
  hack 编译器 builtin）。
- **穷尽性检查**：typecheck 层，对 ADT 与 `List(E)` 的 match 做**充分**条件
  判定（不做 Maranget use-tree）；嵌套构造子只算部分覆盖；未知构造子检查对
  大写名字取三条交集（非 scrutinee 变体 ∧ 非内建类型名 ∧ env 无声明），
  一个 match 至多报一条。引号模式（`'Red`）是符号字面量不是构造子引用，
  不产生诊断（`test/compiler/quote-pattern-clean.ta` 的 stderr 字节断言锁定）。
- **E0006 撞名**：类型名、构造子名与 `pub fn`/`const` 在同一张顶层符号表；
  判定按成员名比（不看 mangle 分隔符）；唯一豁免是构造子与自己类型同名。
- **external fn**：参数位置裸名字 = 无约束类型变量；声明管类型不管元数
  （返回位是变量时元数来自 `builtin_arity`）。没有宿主模块豁免：认一个限定名
  只有两个来源——import 进来的 `external fn` 声明，或 `typecheck.ta` builtin
  env 的承诺（`cov.line` 是唯一的「在承诺表而不在 .ta 文件」项：`--cov` 时
  parser 插桩，`covinst` 在 typecheck 后换成 `(cov.hit k)`，永不到达 VM）。

## 6. 模块内联（编译期）

整程序单 `.tabc`，零 C 改动：driver 在 resolve 阶段把每个模块单元的定义重命名——

| 源码 | 内部名 |
|------|--------|
| `pub fn name` | `mod.name` |
| `fn name`（非 pub） | `mod$name`（`$` 不在标识符字符集，用户语法产不出） |
| `const NAME` | `mod.NAME` |
| `pub type Name` / 构造子 | `mod.Name` / `mod.Ctor` |

模块内部裸引用同步重写：driver 的 `qualify_expr` 复用 lower-ast
`resolve_consts` 的作用域纪律（lambda 参数 / let 绑定 / match·receive 模式绑定
遮蔽成员名）；quoted 符号仅 const 引用与本模块的构造器/类型名被重写；
`qualify_pat` 对模式里的构造子标签做同一件事（值侧与模式侧标签必须拼成同一个
符号）。

非发行版树的单元前缀是 mangle 出来的（带 `~<hash>` 后缀，如
`adt-tag-a~3409857748`，`~` 不在标识符字符集）；诊断渲染一律反查回 import 行
写出的名字，内部名不进给人看的输出。

typecheck 对 dotted 调用按导出表报错：

- 成员存在但非 pub → `[E0004] 'x' is not exported by module 'm'`
- 成员不存在 → `[E0004] module 'm' has no export 'x'`
- 建议拼写用 import 的默认别名（`<树名>:<路径>` 末段），可原样贴回源码

## 7. 自举与构建

```bash
make tinyactor                     # 构建 C VM
./tinyactor build lib/math.ta /tmp/math.tabc  # TA 编译器编译出字节码镜像
make bootstrap                     # 用 lib/bootstrap.tabc 编译 lib/bootstrap/driver.ta
```

- bootstrap fixed point：`make bootstrap` 连跑两遍产物 byte-identical
- 改 `lib/bootstrap/*.ta`（编译器源码）后必须 `make bootstrap` 再跑测试
- `lib/bootstrap.tabc` 入库；`lib/bootstrap/driver.kl` 不入库
- C 模块（`lib/<mod>.dylib` / `.so`）的声明文件 `lib/<mod>.ta` 只在被
  `import` 时参与类型检查；资源值模型（Resource 不透明值、dtor 约束）见
  [c-module.md](c-module.md) §5 与 [c-resource-design.md](c-resource-design.md)