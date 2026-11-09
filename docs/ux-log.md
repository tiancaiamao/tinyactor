# UX log — 使用体验记录

写这门语言的代码时踩到的「不舒适」，逐条追加。**先记，不修**：由维护者周期性
review，决定转 issue / 改设计 / 忽略。规则见 AGENTS.md「用户体验记录」。

条目格式：

```
## YYYY-MM-DD · 一句话标题
- 场景：当时在写什么
- 不适：多花了什么代价（多写、写错、查半天、绕过类型系统…）
- 想要的：如果语言/工具顺手，本该是什么样
- 去向：（留空 = 待 review；采纳后写 → 已解决：<PR/issue>）
```

---

## 2026-11-09 · 多种形状放同一位置，没有类型可写

- 场景：给 `lib/bootstrap/ast.ta` 的 `const_atom` 补类型注解。它把常量摊成裸原子
  —— nil / bool / int / symbol / string / `('float text)` 六种并存。
- 不适：HM 里没有 union，也没有 any。以前靠「match binder 不承重」这个洞蒙混过去：
  写得出，但类型是假的。要把 binder 接通字段类型（#442），这处立刻没有合法写法。
- 想要的：一个「这里本来就是动态」的显式写法。现在有 `Atom` 了（同批落地），
  但**每加一个逃逸口都要掂量它被滥用的成本**——`Atom` 关掉的是下游全部检查。
- 去向：`Atom`（docs/ta-language-spec.md「动态逃逸」），同 PR 落地。

## 2026-11-09 · 返回注解在 match-bodied 函数上不承重

- 场景：读 typecheck 的自测期望，发现「注解 Int 实际返回 String」能过。
- 不适：注解是给人看的契约，静默不生效比没有注解更危险——你会相信它。
- 想要的：注解即约束，违反就报。已随 #442 落地（binder 取构造子字段类型）。
- 去向：→ 已解决：#442

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

## 2026-11-09 · 没跑过 CI 的后端，等于没跑过

- 场景：KL→C 后端本地全绿，接进 CI 后第一轮就炸 `#error requires clang
  musttail`，第二轮又炸 `lib/time.so: undefined symbol: tls_current_proc`。
- 不适：两处都是「macOS 恰好能过」的环境差异（`cc` 不是 clang；Linux 要
  `-Wl,--export-dynamic`），本地跑一万次也看不见。零门禁的代码不能算绿。
- 想要的：新管线第一天就挂进 CI 的既有 job（而不是等「以后加个 job」）。
- 去向：→ 已解决：#446（backend-gate 加 `make test-kl-c`）

## 2026-11-09 · `make test` 全绿不等于 CI 全绿

- 场景：给 `test/basic/` 加了一个用例，本地 `make test` 326 PASS，CI 却红在
  `kernfuzz-snapshot-check`——新语料缺冻结快照。
- 不适：本地没有「CI 等价」入口。`make test` 只串了 11 个 category，CI 另外还跑
  `backend-gate` / `kernfuzz-snapshot-check` / `test-kl-c` / `fmt-check` /
  bootstrap fixed point，得靠记性逐个补跑；忘一个就是一轮 15 分钟的黄灯。
- 想要的：一个 `make pre-pr`（或让 `make test` 直接包含这些 gate），跑完就等于
  跑完 CI。
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

## 2026-11-09 · 从 Gleam 抄一段例子，要逐条改写

- 场景：抄 Gleam 的并发示例到 TA（`pub type` + spawn + `Process.send` + case）。
- 不适：一段 30 行代码改了 8 处，全是语法糖缺口：`#(a, b)` 元组（TA 无）、
  `let #(a, b) = x` 解构绑定（无）、`case`（TA 叫 `match`）、`True/False`（`true/false`）、
  `<>` 字符串拼接（tokenizer 把它拆成 `<` `>`，得写嵌套的 `str.concat`，恰好 2 参）、
  `io.println`（`print`）、`int.to_string`（`str.from_int`）、`Process.new_subject()`
  （TA 的邮箱属于 pid，用 `self()` + 选择性 `receive`）。
  缺的不只是「少写几个字」：元组和解构缺席，让「一个函数返回多个值」只能退成
  `cons(a, b)` + `car/cdr`，而 `cons` 拼出的二元组还会让注解失去精度（见下一条）。
- 想要的：按疼度排序 —— ①`let` 解构/`match` 里直接 `cons(a, b)` 命名 ②变长 `str.concat`
  或 `<>` ③元组字面量。④⑤只是名字差异，记性成本，不算缺口。
- 去向：（待 review）

## 2026-11-09 · `cons(1, nil)` 不是 `List(Int)`，同一份数据两种检查强度

- 场景：给 `lib/list.ta` 全量加注解，然后用 `fn f(xs : List(Int))` 做负例测试。
- 不适：`f(["s"])` 报 `cannot unify List(String) with List(Int)`，`f(cons("s", nil))`
  **不报** —— 内置 `cons : forall(a b, a -> b -> b)`（返回值取尾部类型），所以 cons
  拼的列表是自由 tvar。注解挡不挡得住错用，取决于调用方是用字面量还是 `cons` 建表；
  而 `cons` 恰恰是子列表、pair、AST 节点的标准写法。
- 想要的：`cons(x, xs)` 在 `xs` 是列表时返回 `List(elem)`。现在只能靠「写注解 + 尽量
  用字面量」自我约束。
- 去向：（待 review；已记入 docs/ta-language-spec.md「`List(a)` 的边界」）

## 2026-11-09 · `Pair(101, false)` 不是二元组，`car` 拿到的是符号 `Pair`

- 场景：没有 `#(a, b)` 语法，顺理成章写了 `Pair(101, false)` 想拼二元组。
- 不适：`Pair` 是运行期 Pair 类型名，`Pair(101, false)` 是**构造器调用**形状，实际
  造出 tagged list `(Pair 101 false)`；`car` 返回符号 `Pair`，`car(cdr(x))` 返回 101。
  静默错值（我是在 `str.from_int('Pair)` 打出 `85` 才发现）。真正的二元组是
  `cons(101, false)` → `(101 . false)`。
- 想要的：小写构造器报 `unknown constructor` 就够了，但这里是大写开头的**类型名**
  被当成构造子用 —— 要么给 Pair 一个正经二元组构造子，要么让「类型名当函数调」
  直接报错。
- 去向：（待 review）

## 2026-11-09 · 自己定义 `fn f(a, b)`，import 的库就报假错

- 场景：写测试文件时给辅助函数起名 `f`，同时 `import "tinyactor:lib/list"`。
- 不适：编译失败，错误全在**库内部**：
  `[E0003] in function 'list.map': arg 1 of cons: wrong number of arguments for 'f' (got 1, want 2)`。
  原因是 arity 表（#376）按**裸名**登记全程序函数元数，库内 `map(f, lst)` 的形参
  也叫 `f`，于是库里的 `f(car(lst))` 被按我的 `f/2` 检查。`option.ta`、`dict`、任何
  形参名为 `f`/`g`/`cmp`/`key`/`k` 的库都中招 —— 换句话说：用户不能给函数起名 `f`。
  假错还指向库，第一反应是「库写错了」。
- 想要的：arity 检查只作用于**未被子作用域遮蔽**的全局名（`f` 是 `map` 的形参，
  就该退出全局 arity 表）；或者干脆按符号而不是按名字。
- 去向：（待 review；应与 annotations 分开单独一个 PR 修）
