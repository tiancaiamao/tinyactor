# 管道注入位置：A/B/C 三方案的代价对照（待决策）

状态：**待维护者决策**。本文件只记录现象与代价，不记录结论；定了往「决策记录」
里追加一行，不改正文。

## 现状：一个语言两套约定

`|>` 注**首参**（`parser.ta` 的 `rewrite_pipe`，`a |> f(b)` → `f(a, b)`）。于是：

| 层 | 参数顺序 | 能进管道？ | 例 |
|----|----------|-----------|-----|
| C 原语层（`str` / `dict` / `set` / `json` / `file` / `os` / `net` …） | 数据在前 | ✅ | `s |> str.split(",")` |
| 库 HOF 层（`list` / `option` / `result` / `mpc` / `strconv`） | **函数在前** | ❌ | `list.map(f, lst)` 只能直调 |

`orders |> list.each(fn(o) { … })` 写不出来，`s |> str.split(",")` 写得出来 ——
同一门语言里，管道的可用性取决于调到哪个模块。

## 机制 vs 约定

- **机制几乎免费**：注入点是 `cons(head, cons(left, cdr(right)))` 里 `left` 的位置，
  换边是一行；typecheck / codegen / VM 零改动。
- **代价全在约定上**：TA 没有 labeled arguments，注入位置是**全局常量**。选哪边，
  就要把「带 subject 的函数」全部摆到那一边，没有「按函数指定」的余地。
- 规模基线：272 个 `pub fn`（`lib/*.ta`）、111 个 `external fn`、约 2897 处限定
  形式的模块函数调用（`lib` + `lib/bootstrap` + `test` + `examples`）。

## 三个方案

### A. pipe 保持注首参，HOF 层翻成数据在前（推荐）

- 翻 22 个函数：`list` 11（map / filter / foldl / foldr / flat_map / each / member /
  uniq / chunk / sort / keysort）、`result` 5、`mpc` 4、`option` 2。
- 直接调用点约 87 处：`result.unwrap_or` 19、`option.unwrap_or` 12、`list.sort` 11、
  `list.map` 9、`list.member` 9、`list.uniq` 5、`list.chunk` 4，其余 ≤3。
- `use` 协议跟着改：Gleam 的 `use` 把回调**追加到末位**，TA 现在是柯里化
  `(f a)(lam)`。改成末位后 `mpc.then(p)` 变 `mpc.then(p, k)`，`mpc.then(` 相关的
  约 59 处（`test/module/mpc-*.ta` 41、`lib/json.ta` 11、`lib/csv.ta` 7，其余在
  `test/basic/use-basic.ta`）要一起改。
- 结果：`orders |> list.each(…)` 与 `s |> str.split(",")` 同一套写法，语言只有一个
  约定 —— 读代码时不用先想「这个模块是哪种顺序」。

### B. pipe 改注末参，HOF 层不动

- `parser.ta` 一行；但翻转代价落到 C 原语层：111 个 `external fn` + 全部数据在前的
  `pub fn` + 依赖它们的调用点（2897 这座山的主要部分），连 `lib/*.c` 的导出顺序一起改。
- 且 `use` 追加末位与 pipe 注末位撞在同一个位置：`a |> f(g)` 与 `use x <- f` 都占末位，
  还得再定一套优先级规则。

### C. 只加注解 + 文档写明限制

- 零结构改动；管道只对数据在前的函数可用，HOF 层写不出管道写法。成本是长期的：
  每个新读者都要记住「哪些模块能用管道」。
- 注解本身与 A/B/C 正交，已独立落地（`lib/list.ta` 全量 `pub fn` 注解）。

## 各语言怎么选的（同一个问题的不同答案）

| 语言 | `|>` 注哪 | 机制 | stdlib 约定 |
|------|-----------|------|-------------|
| Haskell（`&`） | 末位 | 柯里化：`x & f y` = `(f y) x` | 数据在末（`map f xs`） |
| F# / OCaml / Elm / Roc | 末位 | 同上（`('a -> 'b) -> 'a -> 'b`） | 数据在末 |
| Elixir | 首位 | 固定元数：`x |> f(y)` = `f(x, y)` | subject 在首 |
| Gleam 1.x | 首位 | 同 Elixir | subject 在首 + 其余参数用 labels 指认；`use` 把回调追加到末位 |
| Gleam ≤0.16（2021 改写前） | 首位 | 同 Elixir | 函数在前、数据在后（与管道相斥） |

关键差别：**Haskell 一系根本没有「注入位置」这个约定** —— 位置由柯里化推导出来。
一旦语言取消柯里化、固定元数（TA / Gleam / Elixir），注入位置就必须写死；而写死的
方向只有一个在表达式里读得顺：首位（`x |> f(y)` 与 `f(x, y)` 逐字对应）。Gleam 2021
年那趟 stdlib 重写（function-first → subject-first + labels）就是为此刻意付掉的
全生态破坏性升级。

**TA 现在的状态 = Elixir 的管道 + ML 的签名顺序**，两套约定同时存在。要收敛的是这个
裂缝，不是管道本身。

## 决策记录

（待填：方案 / 日期 / 理由 / 关联 PR）