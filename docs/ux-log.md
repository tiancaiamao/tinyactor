# UX log — 使用体验记录

写这门语言的代码时踩到的「不舒适」，逐条追加。**先记，不修**：由维护者周期性
review，决定转 issue / 改设计 / 忽略。规则见 AGENTS.md「用户体验记录」。

这个文件是**待办清单**：只放未解决的条目。条目一旦解决（或有明确结论「不做」），
整条原文搬到 [`ux-log-archive.md`](ux-log-archive.md)，不删除历史。
未解决的条目可以重写、拆分、合并 —— 写得含混的条目留在原地只会变成噪音。

条目格式：

```
## YYYY-MM-DD · 一句话标题
- 场景：当时在写什么
- 不适：多花了什么代价（多写、写错、查半天、绕过类型系统…）
- 想要的：如果语言/工具顺手，本该是什么样
- 去向：（留空 = 待 review；解决后整条搬去 archive）
```

只记**用这门语言写代码**时踩的坑。维护者自己的流程摩擦（CI 怎么配、本地怎么跑）
不是这个文件的内容。

---

## 2026-11-09 · 注解里的类型名拼错，报错指向调用点而不是声明

```ta
fn f(s : Strng) -> Strng { s }       // 拼错 String
fn main() { print(f("a")) }
```

```
[E0001] in function 'main' (line 2): arg 1 of f: cannot unify String with Strng
```

- 不适：`Strng` 被当成一个「不透明类型」收下，没有任何「未知类型名」诊断；错误
  冒在调用点，而 typo 在函数签名上。人会先去怀疑 `f` 的实参，而不是自己拼错了。
- 想要的：声明处直接 `unknown type 'Strng'`（与 #443 的 `unknown constructor`
  同一个家族：大写开头的名字总要能对上一条声明）。
- 去向：（待 review）

## 2026-11-09 · 管道的位置和标准库的顺序互相不认

- 场景：把一段 Gleam 抄成 TA，原句是 `orders |> list.each(fn(o) { … })`。
- 不适：`|>` 注首参，而 `list` / `option` / `result` / `mpc` 的 HOF 都是「函数在前、
  数据在后」（`list.map(f, lst)`），管道直接不可用；同一份代码里 `s |> str.split(",")`
  却能用 —— 管道的可用性取决于调到哪个模块，没有任何提示，只能试点一次才知道。
  更糟的是试错是静默的：`rev |> list.map(fn)` 这类换位调用 typecheck 能过（参数
  未注解时），要到运行期才以 `car: expected pair, got tag=0xfffb` 炸。
- 想要的：一门语言一套参数顺序；管道在哪都能用。
- 去向：（待决策，代价对照见 `docs/pipe-arg-order.md`）

## 2026-11-09 · `use` 是柯里化，不是「回调追加到最后一位」

- 场景：写 `use x <- result.try(r)` 期望 Gleam 那样的「回调追加到末位」。
- 不适：TA 把 `use` 脱糖成 `(f a)(lam)` —— 把 lambda 当**返回值**调用。于是
  `use x <- mpc.then(p)` 能用，`use x <- result.try(r)` 报
  `[E0003] wrong number of arguments for 'try' (got 1, want 2)`：两个模块，两种
  协议，写的时候看不出来。跟 Gleam 的差别在根上：Gleam 的 use 依赖 labels 把最后
  一位让给回调，TA 没有 labels，只能靠「函数返回一个吃函数的函数」硬凑。
- 想要的：`use x <- f(a)` 在所有 f 上语义一致。
- 去向：（待决策，与上一条同一个源：参数顺序 + labels）

## 2026-11-09 · 一个函数要返回两个不同类型的值，没有写法

- 场景：写「按 key 分组」的辅助函数，要同时吐出 (key, 累积值)，两者类型不同。
- 不适：没有元组字面量，也没有解构绑定，只能二选一：
  - `cons(k, v)` + `car`/`cdr`：**类型是假的**（`cons` 的结果取尾部类型，见下一条
    「`cons(1, nil)` 不是 `List(Int)`」），注解写什么都盖不住它；
  - 声明一个 ADT（`type KV { MkKV(k, v) }`）：类型准确，但一个临时打包要写 type
    + 构造子 + 每处 `match` 解构，三行样板换两个字段。
  一次性、局部的多值打包，两种成本都偏高，所以实践中大量退化成 `cons` + `car`/`cdr`。
- 想要的：`#(a, b)` 字面量 + `let #(k, v) = ...` 解构（`match` 臂里也直接命名），
  让临时打包和 ADT 各归其位。
- 去向：（待 review；类型层给不给 `Tuple(a, b)` 一条真正的构造承诺，见
  「`Pair(a, b)` 注解没有任何值能满足」）

## 2026-11-09 · 字符串拼接必须恰好两个参数

- 场景：抄 Gleam 的 `"a" <> b <> "c"`。
- 不适：`<>` 被 tokenizer 拆成 `<` `>`（而 `<` 是数字比较），只能用 `str.concat`；
  它**恰好 2 参**，拼 3 段要嵌套两层、多一个中间字符串，拼 N 段（日志行、HTTP 头、
  拼 SQL）几乎不可读。`str.concat("a", "b", "c")` 报
  `cannot unify String with 'a -> 'b`，这个报错也完全指不出「元数不对」。
- 想要的：`<>`，或变长的 concat。
- 去向：（待 review）

## 2026-11-09 · `cons(1, nil)` 不是 `List(Int)`，同一份数据两种检查强度

- 场景：给 `lib/list.ta` 全量加注解，然后用 `fn f(xs : List(Int))` 做负例测试。
- 不适：`f(["s"])` 报 `cannot unify List(String) with List(Int)`，`f(cons("s", nil))`
  **不报** —— 内置 `cons : forall(a b, a -> b -> b)`（返回值取尾部类型），所以 cons
  拼的列表是自由 tvar。注解挡不挡得住错用，取决于调用方是用字面量还是 `cons` 建表；
  而 `cons` 恰恰是子列表、pair、AST 节点的标准写法。
- 想要的：`cons(x, xs)` 在 `xs` 是列表时返回 `List(elem)`。现在只能靠「写注解 + 尽量
  用字面量」自我约束，或者在外面包一层 `fn kons(x : a, xs : List(a)) -> List(a)`
  （这条路今天可用，见「未注解的形参不与注解里的类型变量名绑定」）。
- 去向：（待 review；已记入 docs/ta-language-spec.md「`List(a)` 的边界」）

## 2026-11-09 · `Pair(101, false)` 不是二元组，`car` 拿到的是符号 `Pair`

- 场景：没有 `#(a, b)` 语法，顺理成章写了 `Pair(101, false)` 想拼二元组。
- 不适：`Pair` 是运行期 Pair 类型名，`Pair(101, false)` 是**构造器调用**形状，实际
  造出 tagged list `(Pair 101 false)`；`car` 返回符号 `Pair`，`car(cdr(x))` 返回 101。
  静默错值（我是在 `str.from_int('Pair)` 打出 `85` 才发现）。真正的二元组是
  `cons(101, false)` → `(101 . false)`。全仓已有大量这种写法（`lib/http.ta`、
  `lib/tls.ta` 里 `Pair(status, headers)`），读起来像元组、实际是 tagged list。
- 想要的：小写构造器报 `unknown constructor` 就够了，但这里是大写开头的**类型名**
  被当成构造子用 —— 要么给 Pair 一个正经二元组构造子，要么让「类型名当函数调」
  直接报错。
- 去向：（待 review）

## 2026-11-10 · `Pair(a, b)` 注解没有任何值能满足

- 场景：想给"返回二元组"写精确注解，`fn k() -> Pair(Int, String) { cons(1, "s") }`。
- 不适：两头都过不去，两种写法都被拒：
  - `fn g() -> Pair(Int, String) { cons(1, 2) }` →
    `cannot unify Int with Pair(Int, String)`。因为 `cons : forall(a b, a -> b -> b)`
    **返回值取尾部类型**，`cons(1, "s")` 的类型是 `String`，不是 `Pair`；
  - `fn f(p : Pair(Int, String)) -> String { car(p) }` + `f(cons(1, "s"))` →
    `cannot unify String with Pair(Int, String)`。
  即内建类型名 `Pair(a, b)` 在类型语言里存在（`docs/ta-language-spec.md` 内建类型表里有它、
  `Pair(a, k)` 模式也合法），却是**没有构造函数的类型**。`car`/`cdr` 也只是
  `forall(a b, a -> b)` 的许可证式投影，注解写上的 `Pair` 精度在第一次 `car` 就丢了。
- 想要的：一个能和 `List(a)` 并列的**产品视图**：`Tuple(a, b)` 类型 + 一条真正的
  构造承诺（底层照样是 cons 单元，类型与表示正交）。注意 `cons` 当前承诺
  「结果 = 尾部类型」，所以任何声明成 `Tuple(a, b)` 的**函数体都盖不住** —— 缺的
  恰恰是构造方向那一条内置承诺，投影方向（`fn fst(p : Tuple(a, b)) -> a { car(p) }`）
  今天就能用。或者干脆把 `Pair(a, b)` 从内建类型名里删掉，别留空壳。
- 去向：（待 review；与 `List(a)` 边界同源，见 `docs/ta-language-spec.md`「`List(a)` 的边界」）

## 2026-11-10 · 未注解的形参不与注解里的类型变量名绑定

- 场景：想"不动原语、上层包一个带注解的 fn"来拿到检查（`cons` 的结果是尾部类型，包一层
  `List(a)` 恰好合规）：
  `fn kons(x, xs : List(a)) -> List(a) { cons(x, xs) }`，以为 `a` 同时约束 `x` 和 `xs`。
- 不适：`x` 没写注解，它就是**独立的自由变量**，跟注解里的 `a` 毫无关系：
  `kons("s", [1, 2])` **编过**（实测），元素类型一点都没被检查 —— 包这一层想解决的问题
  原封不动地留着，而且看起来"已经加了注解"。
  写成 `fn kons(x : a, xs : List(a)) -> List(a)` 才对：`kons("s", [1, 2])` 报
  `[E0001] arg 2 of kons: cannot unify List(Int) with List(String)`（实测）。
- 想要的：注解里的同名变量名按「整个签名一个作用域」理解；或者对「注解引用了本函数没声明、
  也不是已知类型的名字」给个提示（现在静默放行，写错和不写看起来一样）。
- 去向：（待 review）

## 2026-11-10 · `Atom` 这个命名

- 场景：给「这里本来就是动态」的位置写标注，#447 落地的名字叫 `Atom`。
- 不适：它的语义是**任意**——与一切类型 unify 成功、且不绑定任何一边，也就是别的
  语言的 `Any` / `Dynamic`。而 `Atom` 在 Lisp/Erlang 传统里指「非复合值」/「符号」，
  正好是「任意」的一个小角落：读代码的人会以为「这位置只放标量」，可它连 Pair、
  函数、Pid 都能装。同批记录的原文里还写着「HM 里没有 union，也没有 any」，转头
  新类型叫 `Atom`，自己跟自己不一致。
- 想要的：叫 `Any`（或 `Dynamic`）。改名是机械替换（两处使用站点
  `lib/bootstrap/ast.ta` 的 `const_atom` / `encode_expr` + typecheck + 规范），
  但它是编译期承诺的名字，错一次要长期读错。
- 去向：（待 review）