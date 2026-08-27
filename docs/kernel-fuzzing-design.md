# TinyActor 内核测试工具链设计（kernel fuzzing）

状态：v4 草案（R1 → R2 → R3 三轮零上下文评审修订；阻塞数 8→3→2→0，
全部条目已修入；评审报告存档：
docs/kernel-fuzzing-review-r1.md / -r2.md / -r3.md）
剩余"实现首日钉死"项均为需跑真实代码才能定的实测点（已在文中逐处标注），
非设计缺失。
范围：语言内核三层 —— L1 表达式/闭包语义×VM、L2 typecheck、L3 GC/scheduler
明确排除：上层库（json/http/net/serve）、第三方库测试、性能 benchmark

v2 主要修订：修正 while 变换（TA 无循环构造）、补齐比对协议/exit code/print 格式三张表、
AST 编码对接 parser-ast.ta 权威表、决策/交付物编号分离（DEC-/DELIV-）、语料库范围定义、
归约判据重定义。

---

## 1. 现状（改之前长什么样）

### 1.1 被测系统结构

| 层 | 文件 | 规模 |
|----|------|------|
| 编译器（TA 自举，跑在 tavm 上） | `lib/tokenizer.ta` / `parser.ta` / `typecheck.ta` / `codegen.ta` / `driver.ta` | 638 / 1606 / 3113 / 1921 / 696 行 |
| C VM | `src/vm.c`(执行) / `src/tavm.c`(CLI 入口/main 进程退出码) / `scheduler.c`(调度/邮箱) / `gc.c`(每进程 semispace) / `val.c`(NaN-boxing) | 1123 / ~150 / 674 / 277 / 289 行 |

关键运行时事实（本次设计过程中逐一核实）：

- **Int 是 int48**：NaN-boxing 低 48 位 payload，`val_int()` 截断、`val_get_int()` 符号扩展
    （`src/val.c:58,147`）。算术走 C int64 再截断，因 2⁴⁸ | 2⁶⁴，结果等价于干净模运算。
  值域 **[-2⁴⁷, 2⁴⁷)**（不对称：-2⁴⁷ 可表示、+2⁴⁷ 不可），溢出静默回绕，spec 未记载。
  另注意**字面量本身也会回绕**：源码字面量超出值域时 `val_int()` 取低 48 位——
  `140737488355328`(=2⁴⁷) 经 parser 后已是 -2⁴⁷。gen 必须禁止越界字面量（§5.0）。
- **混合算术**：任一操作数为 float 则全程 double 且不回缩（`src/vm.c:253-256` 注释）；
  纯整数除法向零截断；除零 → 进程以 `divzero` 原因死亡（`vm.c:296-298`，进程隔离）。
  浮点除零 → IEEE ±inf 不 trap。
- **进程死亡的可观测信号**（`tavm.c:136-141`）：main 进程异常死亡（reason ≠ nil）置
  `main_crashed` → **exit code 1**；正常结束或非 main actor 崩溃 exit code 0。
  返回前有 `fflush+fsync(stdout)`（`tavm.c:139-140`），死亡前的 print 输出保证完整落盘。
- **print 输出格式**（`vm.c:123-160`，golden 必须逐字节复刻的完整算法）：
  int=`%lld` 十进制带负号；float=`%g`；nil/true/false=字面；symbol=名字；
  string=**原样无引号无转义**；pid=`<pid N>`；pair 沿 cdr 链打印 `(a b c)`，
  **链尾非 nil 时输出点对 `(a . b)`**（实测 `(cons 1 2)` 打印 `(1 . 2)`）；
  其他类型兜底 `?`。list 字面量 `[1,2,3]` 运行时打印为 `(1 2 3)`。
  print 自带 `\n` 且每次 flush。
- **字符串转义是语言事实**：TA 字符串支持 `\n \r \t \\ \" \'` 与 `\xNN`
  （test/basic/string-escapes.ta）。v0 的"无转义"仅指 **gen 不产出含反斜杠的
  字符串字面量**，不是语言不支持。
- **main 进程死因枚举**（R3 评审逐字核实 `vm.c` intern 调用）：真实符号为
  `divzero`（除/模零 ×2 处）、`cartype`/`cdrtype`（car/cdr 非 pair）、
  `notafunction`（调用非函数值 ×2 处）、`badopcode`。全部 exit code 1、stderr 静默，
  死因不可从进程外部观测。gen 类型正确性不变式下 v0 子集内唯一可达死因是 `divzero`；
  其余死因若意外出现即为 VM bug，会被锚点断言以输出截断形式捕获（见 §5.1.3）。
- **注意**：`tinyactor run` 对 build 失败同样返回 exit 1（R2 评审实测）——runner 必须
  把 build 与 run 拆成两步，避免撞车（§5.4 调用卡）。
- **语言不可变**（spec「没有可变状态」节）：无 set!/ref/mutable，所有绑定不可变。
  ⇒ 闭包捕获不存在"后续变异可见性"问题，alpha 换名安全。
- **负数字面量的词法事实**（`parser.ta:894-969`）：TA **没有间隔一元负号**——表达式位置的
  `- 5` 中 `-` 是二元 op，会解析失败；负字面量仅在 `use x <- -5` 粘连位置由 tokenizer 折叠。
  ⇒ 生成器产出负数一律用 `0 - N` 形式，绝不输出裸负字面量。
- **TCO 存在**：VM 有 `OP_TAIL_CALL`；CLI 无任何栈深观测选项（--help 已核实）。
- **match 实现**（spec §match）：parser 将 match desugar 为嵌套 `if`+`=` 比较，
  臂序即优先序（首匹配）；guard 用 `when`；**穷尽性缺失只是 stderr warning 不是错误**。
- **fn 两形态**（spec 类型表）：`fn(x){..}` 带参匿名闭包；`fn{..}` **零参匿名闭包**。二者 AST 同构（lambda 节点参数列表为空）。
- **AST 即普通 cons 数据，且有现成规范化打印器**：`test/compiler/parser-ast.ta`
  头注释即文法表（函数调用=以 symbol 为头的 list；块=`('begin ...)`/`('let ...)`链；
  fn 定义=`(define (f) body)`；match 臂=`(pat [guard] body)`），文件内 `render()`
  函数已实现规范 s-expr 文本渲染。

### 1.2 已有测试资产

- 测试套件 ≥8 类全绿：basic/gc/actor/module/compiler/bootstrap/example 七类 +
  `test-cli`（Makefile 目标）+ `test/crash`（run_crash_tests.sh）
- `make bootstrap-selfhost`：自举不动点校验（产物 byte-identical）——单次 50s–2min，太贵，不进高频循环
- `test/compiler/fuzz-regression.ta`：历史 fuzz 战果（parser hang/栈溢出回归 case 列表）
- `test/compiler/compare-parsers.ta`：AST 层等价比对（证明 AST 可被外部消费）
- `test/compiler/parser-ast.ta`：**parser AST 的权威文法参考测试**（AGENTS.md 明确同步义务）
- `tavm_asan`、`http_asan.dylib` 等 sanitizer 构建；ASan 已人工跑过语料 → 本设计只把它当运行底座
- `bench/serve_bench.go`：项目已有 Go 工具链先例（最终未采用 Go，见 DEC-2）

### 1.3 约束（来自 AGENTS.md）

- 一切改动走 feature branch + PR；一问题一分支一 PR
- 分层原则：能在外层解决不下沉到 VM 层（本设计唯一的 VM 层改动是 GC stress 旋钮，见 §7.1 / DEC-5 / DELIV-6）
- `make bootstrap` 成本高 → 所有高频循环必须只依赖 `tinyactor build/run`，不碰 bootstrap
- issue #67 教训（一句话概括）：import 链接的是 lib 当前源码而非 bootstrap.tabc 内嵌版本，
  所以 import tokenizer/parser 的工具测的是工作区版本，可能与 bootstrap 产物不同代——
  ast-dump 属此类工具，须自知这一边界。

---

## 2. 为什么做这件事

1. 内核（语义/类型/runtime）是所有上层正确性的地基，且最难靠手写测试覆盖——组合空间是表达式语法树的笛卡尔积。
2. 手写测试只能覆盖想到的边界；实际 bug 史（fuzz-regression 里整批 parser hang）证明自动化搜索有效。
3. 设计过程已产出第一个实质成果：int48 静默回绕未写入 spec（DELIV-7）。
4. 现有资产（pair 结构 AST + parser-ast.ta 文法表、bootstrap 不动点、sanitizer 构建）让"独立 oracle"建造成本远低于一般语言项目。

核心哲学：bug 不是被找到的，是被逼出来的。我们建造的是让不一致性无处藏身的压力场。

---

## 3. 关键设计决策（编号 DEC-*，与交付物 DELIV-* 分离）

### DEC-1：oracle 组合 = metamorphic 主力 + 黄金锚点（宿主语言见 DEC-6：Python 3）

- Metamorphic（等价变换 fuzzing）：不需要参考实现、全自动、可无限量产。盲区：系统性错误抓不到。
- 黄金锚点钉死绝对正确性，补盲区；覆盖率受子集边界限制 → 只做低频每日轮。
- 高频主力 = metamorphic；锚点每日一轮。

### DEC-2：锚点实现（输入为真实 parser 输出的 AST dump；否决 ta-in-ta）〔宿主语言后经 DEC-6 由 Scheme/Guile 改为 Python 3〕

- TA 的 AST 是 pair/list（golden/sexp.py 的 Symbol/Pair 直接消费）；解释器本体 ≈ 数百行树求值器。
- 独立性边界（精确版）：

  ```
  .ta ──→ tokenizer ──→ parser ──┬─→ typecheck ──→ codegen ──→ tavm 执行 ──→ 结果A
                                 │
                                 └──→ AST(s-expr) ──→ golden 解释器求值 ────→ 结果B
  结果A == 结果B 必须成立（对 §5.0 子集内、typecheck 通过的程序）
  ```

  锚点覆盖 **codegen/VM/GC** 全链路；tokenizer/parser 由既有 mutation fuzz 负责；
  **typecheck 不在本图覆盖范围内**，由 L2 双向 oracle（§6）单独覆盖。
- 否决理由：
  - **ta-in-ta**：解释器与被测程序共享 parser+tavm 底座，VM bug 两边同感染，差分失明（结构性缺陷）。改定位 P2 语言能力里程碑，不进测试工具线。
  - **Go/Python**：需自建 s-expr 解析与树遍历基建；Go 缺尾调用保证。性能在锚点负载（每日千级小程序）下不重要。
- 〔历史记录〕原实现选 Guile ≥3.0；2026-08-27 经 DEC-6 改为 Python 3（仅标准库）。
- 原"尾递归↔while 循环变换依赖 Go 无 TCO"的论证随 T7 变换取消而撤销（见 §5.2）；
  Python 侧深尾递归由 apply_fn trampoline（TCO）+ 大栈工作线程保证同等能力。

### DEC-3：L1 生成器直接产出"全括号 TA 源文本"，而非内部 AST 再渲染

生成器在自己的树结构上做变换，输出时全括号化，规避优先级 bug 引入假阳性。
代价：生成物可读性差——由归约器和强制多行排版兜底（见 §5.5 归约判据）。

### DEC-4：GC 差分拆三个正交手段：顺序子集差分 / 多重集不变量 / TSan 长跑

Actor 并发毁掉全程序确定性，因此不断言并发序列，只断言守恒量。负载×oracle 映射矩阵见 §7.0。

### DEC-5：唯一 VM 层改动 = GC stress 旋钮（分层原则的例外论证）

新增环境变量 `TA_GC_STRESS=N`（每 N 次分配强制当前进程 GC，默认 0 关闭=零行为变化）。
理由：这是测量基础设施而非语言功能；无旋钮则"激进 GC"不可构造，DEC-4 失效。改动集中在分配路径单一调用处。

### DEC-6：工具链宿主语言统一为 Python 3（仅标准库）〔2026-08-27 用户决策，替代原 Scheme/Guile〕
生成器、变换库、锚点、编排脚本共用一套 s-expr 操作原语和值模型；`make`/bash 只做胶水。
宿主从 Guile/Scheme 改为 **Python 3（仅标准库）**：AI 可用性、int48 原生任意精度、`subprocess`
标准件、无 Guile 安装依赖。s-expr 中间表示不变；`interp.scm`/`test-interp-core.scm` 仍保留，
其语义结论（w48/算术语义/match）为 Python 版必须保持的行为基准（`--selftest`/`test_golden.py`
与之一致）。理由：Python 原生整数即任意精度，天然避免 Scheme 侧的 fixnum 双重截断问题。

---

## 4. 总体架构

```
  ┌─────────────┐     ┌────────────────┐
  │ gen.py      │────→│ transforms.py  │──→ 变体 E₁..E₃（各恰施加 1 条变换）
  │ 类型化表达式 │     │ 8 条等价变换    │                     │
  └─────────────┘     └────────────────┘                     ▼
                                   ┌──────────────────────────────┐
                    E₀ ──────────→ │ run.py：tinyactor build+run │
                                   │ （tavm_asan 底座，5s 超时）    │
                                   └──────────┬───────────────────┘
                                              │ 统一比对协议 compare()（§5.1.3）
   AST dump 路径:                              ▼
  ┌──────────────┐   ┌───────────────┐   ┌──────────────┐
  │ ast-dump.ta  │──→│ *.sexp 快照    │──→│ golden/*.py  │──→ 结果B ──→ 与结果A比对
  │ (TA,真实parser)│  │               │   │ (Python 解释器)│
  └──────────────┘   └───────────────┘   └──────────────┘
```

### 目录布局（新建，tools/ 当前为空、无迁移成本）

```
tools/kernfuzz/
  ast-dump.ta          # TA：读入 .ta → tokenize+parse → 按 parser-ast.ta 的
                       #     render() 规范打印 s-expr 文本（复刻其渲染逻辑）
    golden/
    sexp.py            # s-expr reader（AST = Pair 链 / str / int / Symbol）
    golden.py          # 求值器核心（环境模型/closure/int48/match）+ CLI
    test_golden.py     # 单测（翻译 test-interp-core.scm 的 60 断言）
    interp.scm         # Scheme 语义基准（保留，行为基准）
    test-interp-core.scm # Scheme 版断言（60 条，已翻译进 test_golden.py）
  morph/
    gen.py             # 类型化表达式/程序生成器（含边界值注入）
    transforms.py      # 等价变换库（§5.2 Tier A/B）
    run.py             # 批处理编排 + compare() 实现
  typecheck/oracle.py  # L2 双向 oracle 驱动
  gc/workloads.py      # 分配恶劣负载生成（按 §7.0 矩阵分发到三个 oracle）
  gc/multiset.scm      # 并发多重集 harness（含 TA driver 模板生成）
  reduce.sh            # 失败样本归约（判据见 §5.5）
  Makefile.inc         # make kernfuzz-fast / kernfuzz-nightly
build/kernfuzz/        # 运行期产物（gitignore）：
  snapshots/           #   AST 快照 *.sexp；再生成：make kernfuzz-snapshots
  corpus/              #   固定 seed 回归集（fast 环用）；再生成：make kernfuzz-corpus
  findings/            #   失败样本落盘（§5.4 契约）
```

---

## 5. L1：语言语义 × VM（P0）

### 5.0 子集定义（v0）

**语法权威**：每个构造的确切语法以 `docs/ta-language-spec.md` 对应章节为准，
下表给出章节指针与子集裁决：

| 构造 | 语法示例 | spec 章节 | v0 裁决 |
|------|---------|----------|--------|
| int 字面量 | `42`（负数禁用裸字面量，见下） | 基本类型 | ✅ |
| 算术/比较 | `+ - * / %` 与比较运算符全集见 spec《运算符》表（T7 所需 `>` `>=` 在列）；**`==` 对 pair/list 的语义（深比较 vs 引用）实现首日实测钉死**，golden 须复刻 | 运算符表 | ✅（除零走死亡协议） |
| 逻辑短路 | `&& \|\|` | 运算符表 | ✅ |
| bool/nil 字面量 | `true` / `false` / `nil` | 值类型表 | ✅（if 条件、cons 链终止、match 字面量模式需要） |
| if | `if c { a } else { b }` | if 表达式 | ✅ |
| let | `let x = 42; ...`（块内分号序列） | 变量绑定 | ✅（注意：非 `let .. in ..` 记法） |
| 匿名闭包 | `fn(x){..}` / `fn(){..}` 显式空参 / `fn{..}` 零参糖 | 类型表·闭包 | ✅ 三种形态均入子集（T12 对零参函数的包装需要显式空参形态） |
| 顶层具名函数 | `fn f(a: int) -> int { .. }` | 函数定义 | ✅ 可递归；**注解一律小写**（spec 类型表为小写 `int/string/bool/pid`；大写 `Int` 实测被 typecheck reject——R2 评审实测） |
| ADT + match | 见下方 ADT 样例；match 带 guard：`pat when g -> e` | ADT + match 表达式 | ✅ |
| list/pair | `[1, 2, 3]`、`cons/car/cdr`、模式 `[h, ..t]`（**rest 模式语法实现首日以 parser 实测为准**，R3 评审怀疑 spec 模式表未收录 `..t`） | Pair/List | ✅ |
| 字符串字面量 | `"abc"`（语言支持 `\n`/`\xNN` 等转义，但 **gen 不产出含反斜杠的字面量**） | 基本类型 | ✅ 仅作 print 参数与比较 |
| print | `print(expr)` | 内置 | ✅ 唯一副作用 |

**ADT 完整样例**（spec《ADT·声明语法》，可编译形态）：

```ta
type Option { None; Some(value) }
type Shape { Point; Circle(x: int, r: int) }

fn area(s: Shape) -> int {
  match s {
    Point -> 0,
    Circle(_, r) when (r > 0) -> (3 * r),
    Circle(_, _) -> 0
  }
}
// 构造：None 是裸符号值；Some(42)/Circle(1, 2) 是构造器调用（运行时 pair 结构）
```

**接口备注**（A-4，已核实）：`vm.get_arg(0)` 返回**首个用户参数**（非脚本路径）。

**排除（v0 明确不做）**：float 全部路径（v1 需先定 %g 打印对齐协议）、actor 三件套、
string API、const、use/bind（effect 语法）、位运算。

**类型注解规则（Tier B/C 的前置事实，R2 评审实测）**：spec **不支持箭头类型注解**
（`g: fn(int)->int` 直接 type error）。因此：函数类型的绑定（参数/let 值）**一律省略
注解、由 typecheck 推断**；只有 int/string/bool 等基本类型可写注解。
gen 的类型模型必须编码这条规则。

**负数书写规则**：TA 无间隔一元负号（§1.1 词法事实），生成器输出负值一律 `(0 - N)`；
T4 折叠出负值时同样渲染为 `(0-abs)`。**字面量绝对值 ≤ 2⁴⁷-1 是 gen 硬不变式**
（越界字面量会被 parser 静默回绕，意图值错位）。边界值集合：
`0, 1, -1, 140737488355327(=2⁴⁷-1), (0 - 140737488355327 - 1)(=-2⁴⁷)` 及 ±小扰动。
T4 折叠以 AST 内已回绕值为准。

**作用域规则**（spec《变量绑定》，golden 环境模型的实现依据）：
- let 在函数体内是**顺序扁平绑定**，无嵌套 let 作用域语法；后续绑定可引用前面的
- 遮蔽的唯一形式是**同名顺序重绑定**（`let x = 1; let x = (x + 1)`）——遮蔽链探测据此修正
- 顶层只能定义函数和类型，无顶层 let
- 顶层定义间互递归是否可见：**实现首日核实**（spec 未明说，用两行互调样例实测）

**gen 终止性不变式**（T6/T10/T11 正确性的前提）：gen 只产出必终止程序——
递归必须有结构性递减参数、禁止自引用 let 值。§5.2 各变换的正确性均以此为大前提
（否则内联/展开改变求值次数会破坏等价，制造超时假 finding）。

**语义钉死项**（golden 实现前必须与 spec/tavm 行为三方一致）：
- int48 回绕：结果归一到 [-2⁴⁷, 2⁴⁷)
- `/` `%` 向零截断：Python 侧用 `int(a/b)`（向零）与 `a - int(a/b)*b`，**禁用 Python `//`/`%`（floor 语义）**
- 除零：走 §5.1.3 死亡协议，不是值
- match：臂序即优先序（首匹配，desugar 为嵌套 if）；guard 先于体求值；
  穷尽性缺失仅 warning → **生成器产出的 match 必须自带通配臂或生成器证明穷尽**，
  否则 L1 会把 warning 当噪声
- 闭包捕获：语言不可变（§1.1），捕获即环境链共享，无变异可见性问题

**最小完整示例程序**（gen 输出形态的规范样例，注解小写、函数类型绑定无注解）：

```ta
fn add(a: int, b: int) -> int { (a + b) }
fn fact(n: int) -> int {
  if (n <= 1) { 1 } else { (n * fact((n - 1))) }
}
fn twice(g, y: int) -> int { (g(y)) }   // 函数类型参数 g 无注解，靠推断
fn main() {
  let x = 3;
  print(add(x, 4));
  print(fact(5));
  print(twice(fn(z: int) -> int { (z * 2) }, 21));
  let lst = [1, 2, 3];
  print(match lst {
    [h, ..t] -> h,
    _ -> 0
  })
}
```

（实现第一步：本样例跑通 ast-dump + `tinyactor build`，任何一步失败即 spec 认知有误，
先修认知再动工。）

### 5.1 程序形态与三张协议表

#### 5.1.1 程序骨架

```ta
// 单 actor、确定性、纯表达式
fn helper_1(a: int, b: int) -> int { ... }
...
fn main() {
  print(<expr_1>);
  ...
}
```

#### 5.1.2 exit code 与结局对照表（实测于 tavm.c/tavm，实现时以此为准复核）

| 结局 | stdout | exit code | stderr 特征 |
|------|--------|-----------|------------|
| build 失败（typecheck reject 等） | — | 1（wrapper 透传） | 错误文本 |
| 正常结束 | 全部 print 行 | 0 | 空 |
| main 死亡：divzero / notafn / carerr / cdreerr / badop（§1.1 枚举） | 已打印行（flush 保证完整） | 1 | **空**——死因不可从外部区分！|
| panic/VM 崩溃 | 截断/不定 | ≠0 且非 1 | panic/ASan 特征文本 |
| 超时(hang) | 截断 | 被 runner kill | — |

死因不可观测 ⇒ 协议层不区分死因，靠两条不变式兜住：
(a) gen 类型正确性不变式使 divzero 成为唯一可达死因（§1.1）；
(b) 若其他死因意外发生（= VM bug），输出提前截断 → 与 golden 的行数对不齐 →
锚点断言必然报 mismatch，只是失败类别可能被记为 mismatch 而非 unexpected-divzero
（分类学瑕疵，不影响检出力；finding 报告附 stdout 全文供人工归类）。

#### 5.1.3 统一比对协议 compare()（morph 与 golden 共用的唯一判定入口）

```
norm_tavm(stdout, run_exit_code):        # 注意：run 阶段的 exit，build 已先行成功
  lines ← split(stdout, "\n") 去尾部空行
  if run_exit_code == 1: append(lines, "DIVZERO:" + len(lines))   # runner 合成协议行
  return lines

norm_golden(golden_stdout):        # golden 自己在 divzero 时打印 DIVZERO:<n> 末行
  return split(golden_stdout, "\n") 去尾部空行

compare(P 的两个结局): 逐行字节相等（不做空白归一）
```

要点：DIVZERO 协议行不是特例分支而是 norm 的一部分；
两侧 n 都等于"成功打印的行数"，天然对齐；stdout 差半行即 mismatch。

### 5.2 等价变换库（transforms.scm）

按抽象层级分两档。Tier A 在**算术代数**层面重写（抓求值/回绕类 bug）；
Tier B 在 **λ-结构**层面重写（抓调用约定/闭包环境布局/arity 处理类 bug，与 Tier A 互补）。
前提事实（已核实）：函数是一等值——裸名引用合法（`f(1)(2,3)` → `((f 1) 2 3)`，
parser-ast.ta:255）、closure 是 NaN-boxing 原生类型（TAG_CLOSURE）。

#### Tier A：算术代数（8 条）

| # | 变换 | 正确性条件 |
|---|------|-----------|
| T1 | 加法/乘法交换律 `a+b↔b+a` | 恒成立（int48 模运算交换） |
| T2 | 结合律重分组 `(a+b)+c↔a+(b+c)` | 恒成立 |
| T3 | 单位元 `x+0↔x`、`x*1↔x`、`x-x↔0`、`(0-x)+x↔0` | 恒成立 |
| T4 | 常量折叠 `<lit op lit>↔<值>` | 按 int48 折叠；负值渲染 `(0-abs)` |
| T5 | `if true {x} else {y}↔x`（及 false 对称） | 恒成立 |
| T6 | let 绑定内联 `let x=e; body↔body[e/x]` | 仅当 **e 不含 `/`、`%`**（或 x 在 body 中被引用 ≥1 次）才内联：零引用且 e 含除法时，原程序求值 e 一次可能死、变体不执行 e 存活，求值次数不等价；语言不可变故无赋值风险；内含 α 换名 |
| T7 | 比较对偶 `(x < y)↔(y > x)`、`(x <= y)↔(y >= x)`、`(x == y)↔(y == x)` | int 全序恒真 |
| T8 | match 臂重排 | 可执行谓词：**通配臂存在 ⇒ 仅允许它处于末位且只交换其前的非通配臂；无通配臂 ⇒ 要求 gen 在臂节点上标记 pairwise-disjoint（构造器两两不同或字面量值互异）才允许重排**。标记存于 gen 树的 match 节点元数据，transforms.scm 查询该字段 |

> v1 曾计划"尾递归↔while 循环"变换（T7 旧）：**TA 无任何循环构造，已取消**。
> 其原本想探测的 TCO 性质改由定向探测 + Tier B/C 承担。

#### Tier B：λ-结构变换（4 条，P0 后半段接入）

| # | 变换 | 正确性条件 | 主要探测面 |
|---|------|-----------|-----------|
| T9 | α 换名：某 let 重绑定/fn 参数整体换 fresh 名（所有引用点同步） | 语言不可变 + 卫生换名；作用域规则见 §5.0（扁平顺序绑定，无嵌套作用域语法） | 符号表/作用域解析、遮蔽处理 |
| T10 | β 归约：`(fn(x){body})(e) ↝ body[e/x]`（先 α 防捕获） | v0 纯表达式无副作用 + gen 终止性不变式（§5.0），恒真 | 直接调用 vs 绑定路径的一致性 |
| T11 | β 展开（引入 redex）：恒等包装 `(f a) ↝ ((fn(g){g})(f) a)`；或对任意子式 `(fn(x){x})(e)` 包装 | 严格求值纯语言下恒真（e 求值次数不变） | 调用约定、参数传递、栈帧布局 |
| T12 | η 展开：函数值 `f ↝ fn(x₁..xₙ){ f(x₁..xₙ) }`（n=被包函数 arity；n=0 时用显式空参形态 `fn(){ f() }`——已实测存在）；η 归约即反向 | 同 arity 包装恒真 | 闭包环境布局、间接调用层、TCO 保持（包装后深递归仍须不爆栈）|

**Tier B 类型注解铁律**：包装用到的 fn 参数（如 T11 的 `g`、T12 的 `x₁..xₙ`）
**一律不写注解**（spec 不支持箭头类型注解，§5.0）——省略注解靠 typecheck 推断。
Tier A 骨架的基本类型注解（`: int`）与此不冲突。Tier C 的 CPS 中间函数同理。

生成器配套要求：gen 需能产出函数值位置的子表达式（匿名闭包字面量、裸顶层函数名、
let 绑定函数值）——这是 Tier B 带来的唯一子集扩充点，实现前先用
`fn callf(g){ g(41) } callf(fn(z: int) -> int { z })` 两行样例验证编译通过（R2 已实测可行）。

#### Tier C：CPS 全程序变换（P1，独立里程碑 DELIV-11）

把整个 L1 程序做 CPS 变换后执行，输出必须与原程序逐行一致。价值与定位：

- **终极差分**：变换后的程序在闭包密度、调用深度、环境共享结构上与原程序截然不同，
  codegen/GC/closure 布局类 bug 几乎必然暴露——是局部重写无法替代的重锤
- **免费搭车 TCO 性质检验**：CPS 化程序全部调用皆尾调用，tavm（OP_TAIL_CALL）与
  golden（TCO trampoline）两侧都必须栈有界，否则超时报警
- 不进 P0 的理由：(a) 它是全程序变换，需操作 gen 的 AST 树而非源文本，与现有
  per-expression 变换框架不同构；(b) 变换器自身实现复杂度高，其 bug 会制造假阳性——
  必须先有稳定的 runner + 锚点当裁判，才能驯服 CPS 变换器自身的开发风险
- 上线门槛（自检）：CPS 变换器先对子集语料全量跑「变换前后 golden 输出一致」，
  全绿后才允许上线抓 bug

定向探测（混入生成分布，非变换）：
- 边界值注入（§5.0 负数规则下的 int48 回绕高发区）
- 遮蔽链：同名**顺序重绑定**序列 + 闭包捕获（let 是扁平绑定，遮蔽=重绑定；closure-overwrite-scope 历史 bug 模式）
- 深尾递归：单个 10⁶ 层尾递归调用程序，断言正常结束（OP_TAIL_CALL 栈有界的直接性质检验）

### 5.3 黄金锚点（golden/，宿主 Python 3）

黄金锚点现以 Python 3 实现（见 DEC-6），文件为 `golden/{sexp.py,golden.py,test_golden.py}`；
`interp.scm`/`test-interp-core.scm` 为 Scheme 语义基准（其 60 断言已翻译进 `test_golden.py`）。

- **ast-dump.ta**：import tokenizer/parser，复刻 `parser-ast.ta` 的 `render()` 渲染逻辑
  输出整个文件的 AST s-expr 文本。
  **接口契约**：目标路径取自 argv（TA 内建 `vm.get_arg(0)`），读文件用内建
  `file.read(path)`（driver.ta:573 同款用法，R2 复核存在）；结果打印 stdout。
- **s-expr 编码表**（与 parser-ast.ta 头注释一致，该文件为唯一权威，快照防漂移）：

  ```
  int → 42          string → "hi"（带引号，转义解码）    symbol → name
  nil → nil         true/false → true/false
  list → (a b c)    dotted pair → (a . b)
  节点形态：函数调用=(f a b)；块=('begin ...)；let=('let x val body)；
           fn 定义=(define (name params...) body)；lambda=(lambda (params...) body)；
           match 臂=(pat [guard] body)
  ```

  实现第一步：通读 parser-ast.ta 全部 cases() 用例，将上述清单扩成完整节点表并冻结
  （快照存档，dump 逻辑变更必须显式重建）。
  非法源码（`*-errors.ta` 等）不在快照范围，dump 若意外成功即为报警项。
- **interp.scm 关键约束（Python 版须保持一致）**：全程使用任意精度整数，**只在每次算术运算后调 w48 归一**，
  绝不用 fixnum/int64 做中间量（否则双重截断）。除法向零截断（`int(a/b)`），remainder
  用 `a - int(a/b)*b` 实现（符号随被除数）。

  规范实现（Python）：
  ```python
  def w48(n):
      m = n % (1 << 48)
      return m - (1 << 48) if m >= (1 << 47) else m
  ```

- main 有返回值时不打印返回值（与 tavm 的 eval_result 行为对齐——实现首日验证）。
- 求值遇除零 → 打印已完成的行后追加 `DIVZERO:<行数>` 末行，交由 compare() 对齐。
- **golden print 规范**（逐字节复刻 §1.1 的 print_val 算法，含全部分支）：

  ```
  print_val(v):
    int    → 十进制带负号（v 已是 w48 归一后的有符号值）
    nil/true/false → 字面
    symbol → 名字原样
    string → 解码后的字节原样（无引号无转义）
    pair   → "(" + car 递归 + 沿 cdr 链循环打印 " " + car 递归
             + 若链尾非 nil：追加 " . " + 链尾递归
             + ")"
    pid    → "<pid N>"
    其他   → "?"
  ```

  注意 list 字面量 `[1,2,3]` 运行时打印 `(1 2 3)`；dotted pair `(1 . 2)` 是必测形态
  （gen 会产出 cons 非规整结构）。
- **string 的 dump↔print 双向规则**：dump 时重新编码转义（`"`→`\"`、`\`→`\\`、
  控制字节→`\xNN`）；print 时输出解码后字节。gen 不产出含转义的源码字面量，
  但 AST 快照中来自语料的字符串可能含转义——两条规则都要实现。
- **golden.py 输入输出契约**：目标 sexp 文件路径经 argv 传入、协议行输出 stdout：
  `python3 golden/golden.py <path.sexp>`

### 5.4 runner 协议（morph/run.scm）

**管线记号**（消解"变换对象是树还是文本"歧义）：
`tree₀ = gen(seed)` → `treeₖ = apply(tₖ, tree₀)` → `srcₖ = render(treeₖ)`
（全括号多行排版）→ `run(srcₖ)`。变换只操作树；渲染只在 run 前。
下文 E₀/Eₖ 指 tree；src 指 render 产物。

**runner 调用卡**（两步走，杜绝 build-fail exit 1 与 crash exit 1 撞车）：

```
构建：tinyactor build src.ta -o <artifact>     # exit≠0 → build-fail 类别，不进比对
运行：<asan tavm 二进制> <artifact>            # 具体产物路径/asan 目标名实现首日钉死
                                                 # ASan exitcode 约定（R3 C-1）：runner 以
                                                 # ASAN_OPTIONS=exitcode=42 启动 asan 底座——
                                                 # 否则 ASan 报错默认 exit 1，会被死亡协议
                                                 # 误合成 DIVZERO 行；exit==42 一律记 tavm-crash
                                                 # 并附 ASan stderr 全文
超时：5s（kill 后归 hang 类）
采集：stdout / stderr / exit 三元组
```

```
for seed in batch:                       # gen 必须对 seed 确定（同 seed 同程序，去重前提）
                                         # PRNG 约定（M-2）：禁用宿主内建 random（跨版本不稳定），
                                         # 自实现 counter-based 流：state=seed，每次取
                                         # sha256(state||计数器) 前 8 字节为随机数并 state++，
                                         # 纯 Python 约 20 行，跨版本 bit 级可复现
  tree₀ ← gen(seed)                      # 强制多行排版（每 print 一行，禁止单行长表达式）
  variants = []
  for k in 1..3:
    ts ← applicable_transforms(tree₀)
    若 |ts| < 3：重 roll 子 seed 重生成该程序，重试上限 5 次，仍不足则记 skip
    （skip 不计入验收分母）
    t   ← random_pick(ts)                # 恰好 1 条变换，不叠加
    treeₖ ← apply(t, tree₀)
    variants += [(treeₖ, path=[t.id, t.target_node])]
  for (P, _) in [(tree₀,[])] ++ variants:
    (out, exit) ← build_and_run(render(P))   # 见调用卡，timeout 5s
    assert compare 全体一致                        # metamorphic 主断言
                                                 # 比较拓扑（A-6）：星形——E₀ 与各变体逐一比，
                                                 # 变体间不互比（每变体恰 1 变换，与 E₀ 的差异
                                                 # 即该变换的效应）；锚点断言仅作用 src₀/E₀，
                                                 # 变体不跑 golden（有意省略，控制成本）
  B ← golden(dump(src₀)); assert compare(norm_golden(B), norm_tavm(out₀, exit₀))   # 锚点断言
失败分类学（签名的类别域，封闭枚举）：
  mismatch | tavm-crash | anchor-crash | hang(超时=finding，非 skip——hang 是本线历史主要战果)
  | unexpected-divzero | dump-fail | build-fail
签名 = (类别, sha256(strip_ws(源码)) 前 16 hex) 二元组字符串；同类已知 finding 自动跳过
findings/<类别>-<hash前8位>/ 落盘契约：源码、seed、变换路径、E₀ 与各变体的
stdout/stderr/exit、golden 输出、ASan 报告（若有）、复现命令行
```

### 5.5 归约器（Python 实现，`reduce.sh` 仅作入口）

- **实现语言（R3 C-2 修正）**：策略 2"表达式子树替换为字面量"需要 AST 操作，
  bash 无 parser、失败样本只落盘源码文本+sha 无法回溯 gen 参数——故 reduce **主体
  用 Python 实现**（直接 parse 源码文本得 Pair 树操作，复用 golden/sexp.py），
  `reduce.sh` 只做参数转发入口，与 DEC-6"Python 统一、sh 只做胶水"一致。
- **复现判据**：失败**类别**不变 **且** 根因特征匹配——mismatch 类要求差异行位置相同；
  crash 类要求 stderr 特征串相同。**不要求完整签名哈希一致**（归约必然改源码哈希）。
- **crash 特征串归一化（M-12）**：取 stderr 首行，将 `0x[0-9a-fA-F]+` 全部替换为
  `0xADDR` 后作为特征串（panic 文本中的文件名/行号对同一程序是确定的，仅地址漂移）。
- 策略：整行删除（生成物强制多行排版保证此步有效）→ 表达式子树替换为字面量 → delta-debugging 式 token 删减。
- 行数指标：目标 ≤15 行为**尽力而为**（best-effort），不设硬验收门槛；
  验收只要求"归约后仍满足复现判据"。

### 5.6 工具链自检（P0 验收门槛，防"静默失效永远绿灯"）

- 人为去掉 interp.scm 的 w48 → 边界值用例必须批量报警
- 人为破坏 T3（写成 `x*0↔x`）→ runner 必须报出对应 seed
- 人为让 ast-dump 漏一种节点类型 → golden 对含该节点的程序必须报 anchor-crash/dump-fail

---

## 6. L2：typecheck 双向 oracle（P0）

### 6.0 typecheck 调用接口

typecheck 是 `tinyactor build` 流水线的一环（`lib/driver.ta:364-368`）。
三种结局的可观测特征：

| 结局 | 特征 |
|------|------|
| accept | exit 0，无 `type error(s)` 输出行 |
| reject | exit ≠0，输出含 `typecheck: N type error(s) found` 类行 |
| crash | panic 文本 / 信号死亡 / ASan 报告（**崩溃本身就是 finding**） |

（实现首日：用一个故意类型错误的样例程序实测三种特征并冻结进 oracle.scm 常量表。）

### 6.1 健全性方向（正例）

```
对 gen 产出的良构程序 P：
  A1: build 必须 accept
  A2: 编译+运行不得崩 VM（panic/信号/ASan 报告都算失败）
```

（原 v1 的"运行结果与 gen 预期值抽查"已删：gen 对含闭包程序预言结果等于内置半个求值器，
与 golden 职责重复；绝对正确性由 L1 锚点断言覆盖，此处不再重复。）

### 6.2 完备性方向（负例 = 对 P 施加保类型破坏）

| 变异 | 期望 |
|------|------|
| 字面量换型（Int 位臵塞 String） | reject |
| 函数实参数量/类型错配 | reject |
| 引用未定义变量 | reject |
| 构造器字段类型错配 | reject |
| ~~删除 match 某臂~~ → 改为**穷尽性警告一致性检查** | 因穷尽性缺失只是 warning 非 reject（§1.1），该变异单独归类：断言 stderr 出现 `non-exhaustive match` warning 且 build 仍 accept；若某天行为变为 reject，此检查立即报警（双向守护） |

断言三连：exit ≠0 **且** stderr/stdout 含错误类关键词 **且** 非 panic。
对照组：未变异的 P 必须通过（防生成器产垃圾导致假绿灯）。
**变异前置校验**：每个负例入库前必须先过 parse 检查（dump 成功）——意外制造
parse error 的变异归入独立的 `parse-reject` 类别，不计入 reject 断言分母
（typecheck 与 parser 的错误文案不同，混在一起会污染断言）。

### 6.3 推导元性质

- **确定性**：同一文件连续 build 两次，诊断输出**逐字节**一致
- **顺序无关性**：随机打乱顶层非 main 定义顺序 → **仅 accept/reject 结论**不变
  （有意比确定性粒度粗：诊断行序可能随符号表遍历序变化，不算翻转）
- **fmt 幂等性**：`compile(fmt(P)).tabc == compile(P).tabc`（byte 级）。
  **前提：codegen 确定性**——DELIV-9 第一项验收即验证之（同一输入连续编译两次产物 byte 相等），
  若发现时间戳/地址随机性则此条降级为结构相等并记录。

---

## 7. L3：GC / scheduler（P1）

### 7.0 负载 × oracle 映射矩阵（消解"workloads 服务对象"歧义）

workloads.scm 产出三类负载，各自只能喂给指定 oracle：

| 负载类 | 形态 | 喂给 | 断言 |
|--------|------|------|------|
| W-pure | 单 actor、纯分配（闭包捕获即弃、深共享结构） | GC 顺序差分（stress vs normal） | 输出逐行一致 |
| W-msg | 多 actor、消息 churn、跨 send 共享 | 多重集 harness | 消息多重集守恒 |
| W-chaos | spawn/death 高频 + 消息混合 | TSan 长跑 + 超时检测 | 无 data race 报告；不超时 |

W-pure 严格不含 send/spawn（保住顺序差分的确定性前提）。

### 7.1 GC stress 旋钮（前置交付物，DELIV-6）

`TA_GC_STRESS=N`：每 N 次 alloc 强制当前进程 GC；`=1` 最暴力；默认 0 关闭。
实现点：进程分配路径单一调用处；不改语义；ASan/TSan 构建同样生效。

### 7.2 顺序差分（GC 逻辑正确性主 oracle）

```
对 W-pure 负载 P：assert tavm run P 输出 == TA_GC_STRESS=1 tavm run P 输出（compare()）
```

### 7.3 并发多重集 harness（multiset.scm）

骨架（TA driver 由 Python 模板生成，K/M 可参数化）。
**spawn 语法事实（R3 评审核实 spec《Actor 模型》章）**：spawn **只有零参形态**
`spawn('fn_name)` / `spawn(fn{..})`，**不支持向被 spawn 函数传参**。worker 的参数
(collector_pid, i, m) 必须走**首消息配置**模式：spawn 后立即 `send(pid, [cp, i, m])`，
worker 先 `recv()` 取配置再进入工作循环。`send` 载荷支持任意 TA 值且深拷贝
（spec Actor 章表格），两个 int 打包为 list `[cp, i, m]` 即可；collector 终止 =
函数返回即进程正常死亡（实现首日核实"正常返回 vs 显式 HALT"的语义）；行拼装用
`str.concat` + `str.from_int`。

```ta
// collector：收满 K*M 条后逐行打印，然后结束
fn collector(n_total: int) {
  var 形式省略——用递归计数收消息
  // 每收一条：print("<wid> <seq>")   ← 只打印，不在 TA 内排序
}
fn worker() {
  // 首消息取配置：let cfg = recv();  → [cp, i, m]
  // for s in 0..m-1: send(cp, 消息)  ← 用尾递归展开
}
fn main() {
  let cp = spawn('collector);
  send(cp, K*M);            // collector 的 n_total 也走首消息
  spawn('worker); send(<pid0>, [cp, 0, M]);
  // ... K 个 worker 同理（顺序展开，Python 模板生成）
}
```

判定在 runner 侧完成：收集 stdout 行 → sort → 与预期枚举（笛卡尔积 K×M 逐行生成）比对。
超时（10s）= finding（丢消息与死锁二选一，均报）。排序摘要刻意放在 shell/Python 侧，
TA 程序保持极简（避免被测代码自身复杂度过高引入假阳性）。

### 7.4 竞争与活性（独立战线）

TSan 构建（`TSAN=1 make tavm`，Makefile:24-35）+ W-chaos 长跑（nightly，30min 上限）；
超时检测死锁；fairness 统计断言列为 P2 观测项。

---

## 8. 交付物清单（DELIV-*，按 PR 拆分）

| ID | 交付物 | 层 | 优先级 |
|----|--------|----|--------|
| DELIV-1 | `ast-dump.ta` + AST 节点表冻结 + 语料快照 | L1 | P0 |
| DELIV-2 | `golden/` Python 解释器 + **子集内**语料全量通过 | L1 | P0 |
| DELIV-3 | `morph/` 生成器 + Tier A 8 变换 + runner + 自检（Tier B 4 条 λ-变换随后接入） | L1 | P0 |
| DELIV-4 | `reduce.sh`（复现判据见 §5.5）+ 签名去重 | infra | P0 |
| DELIV-5 | `typecheck/oracle.scm` 双向 + 元性质 | L2 | P0 |
| DELIV-6 | `TA_GC_STRESS` 旋钮（VM 层，单独 PR 论证） | L3 | P1 |
| DELIV-7 | spec 补充：int48 语义章节（含一句 VM 侧算术依赖 -fwrapv 类保证的说明——C 有符号溢出是 UB，模运算等价性依赖实际代码生成行为） | docs | P0 |
| DELIV-8 | `gc/` 顺序差分 + 多重集 harness | L3 | P1 |
| DELIV-9 | fmt 幂等性接入快速环（含 codegen 确定性前提验证） | cross | P0 |
| DELIV-10 | Makefile.inc + nightly 接线 | infra | P1 |
| DELIV-11 | Tier C：CPS 全程序变换器 + 语料自检上线门槛（§5.2） | L1 | P1 |

延迟项（明确不做及理由）：HTTP parser libFuzzer（库层）、json.ta 差分（库层）、
float 进锚点（v1，需 %g 打印对齐协议）、ta-in-ta（P2 能力里程碑）、fairness 断言（P2）。

## 9. CI 分层

- **快速环**（push 触发，`make kernfuzz-fast`）：
  morph **300 固定 seed 基准集 + 200 滚动新 seed = 500 基础程序（×4 执行单元）**、
  fmt 幂等性扫语料、typecheck 固定 seed 负例回归集（固化快照，不现生成）
- **快速环预算（R3 C-4）**：500×4 次执行的最坏情形远超 5min——故 fast 环单程序
  超时上限降为 **2s**、且 build 产物按 seed 缓存复用；**实现首日实测单次 build+run
  耗时并回填下表，若仍超 5min 则按比例缩减 fast 规模**（预算表：`docs` 首日补，
  格式 `build X ms / run Y ms / 500×4×(X+Y) = Z s`）。
- **慢速环**（nightly，`make kernfuzz-nightly`）：golden 子集语料全量 + 1000 生成程序、
  typecheck 双向 2000 例（现生成）、GC 顺序差分、多重集 harness、TSan 长跑
- **Tier 覆盖矩阵**：fast=Tier A；nightly=Tier A+B+golden+GC；CPS(Tier C) 上线后进 nightly
- **滚动 seed 派生规则（M-7，可直接抄）**：
  ```python
  # 字段拼接格式：sha256(<git_sha>:<YYYY-MM-DD>:<counter>)，':' 分隔、固定顺序
  # counter 为该 ring 当日递增整数，持久化于 build/kernfuzz/rolling-counter
  def roll_seed(git_sha, date, counter):
      h = hashlib.sha256(f"{git_sha}:{date}:{counter}".encode()).digest()
      return int.from_bytes(h[:8], "little") % 140737488355327  # 折进 int48 正值域
  ```
  可复现（同日同 commit 同 counter 同程序）、可追溯（失败报告必记 seed 全值）
- **产物入库 vs 再生成（M-10）**：`build/kernfuzz/` 整体 gitignored 是对的，但其中
  三类"冻结/固化"产物需跨 clone 可得，逐项裁定——**入库 git**（移至
  `test/kernfuzz-frozen/`，不进 build/）：AST 快照、typecheck 负例固化快照、
  expected-fail 清单、固定 seed 基准集清单（仅 seed 列表，程序由 gen 确定性再生成）；
  **每次再生成**：语料源码、.tabc、日志。再生成的确定性依据 = §5.4 PRNG 约定。
- **平台矩阵（M-13）**：日常开发 macOS arm64；**TSan 战线与 nightly 接线以 Linux x86_64
  CI runner 为准**（TSan 在 macOS arm64 支持长期受限，不在 macOS 上承诺 TSan 绿灯；
  实现首日在 Linux runner 上验证 `TSAN=1 make tavm` 可用后回填本条）。
- **fmt 幂等性工作流**（fmt 是**就地改写**，不能直接 fmt 原件）：
  ```
  cp P.ta P.fmt.ta && tinyactor fmt P.fmt.ta
  tinyactor build P.ta     -o build/a.tabc
  tinyactor build P.fmt.ta -o build/b.tabc   # build 支持 -o 指定产物路径
  cmp build/a.tabc build/b.tabc              # byte 级相等
  ```
  （`.tabc` = TA 字节码产物。）
- **工具链缺席时的退出语义**：fast 环 → 打印 `KERNFUZZ-SKIPPED: 工具链缺失` 且 exit 0
  （CI 不红但显式留痕）；nightly → exit 1（慢速环不允许静默跳过）
- bootstrap 不动点维持手动触发，不入环

---

## 10. P0 验收场景

### DELIV-1: AST dump
1. `tinyactor run tools/kernfuzz/ast-dump.ta test/basic/closure.ta` → 输出 s-expr，重复运行逐字节一致
2. 快照范围 = test/basic/*.ta ∪ test/compiler/ 合法程序。合法判定：文件名排除
   `*-errors*`/`*-parse-errors*`，**且 dump 成功**；dump 失败者进 expected-fail 清单
   人工复核一次后清单固化（此后 dump 失败即报警）。一次性人工验证步骤：临时改动
   parser 一个打印可见的节点结构 → 重跑 diff 非空 → 还原（一次性 demo，不入 CI——
   CI 化需要 fixture 化恶意 parser，成本不成比例）

### DELIV-2: 黄金锚点
1. §5.0 子集内的语料程序筛选：typecheck accept 且 **dump 出的 s-expr 全树遍历、
   收集所有 list 节点的 car（symbol 者）与冻结节点表求差 = 空**；golden 与 tavm 输出
   经 compare() 全部一致，0 mismatch
2. 边界回绕卡（真实源码）：`print(140737488355327 + 1)` → 两侧同为 `-140737488355328`
3. 向零截断卡：`print(7 / (0 - 2))` → 两侧同为 `-3`
4. 除零协议卡：`print(1 / 0)` → 两侧同为 DIVZERO 协议行为（exit 1 + 协议行）
5. 自检：去掉 w48 → 边界用例批量报警

### DELIV-3: metamorphic runner
1. `seed=42 batch=100`：100 基础程序 × 各恰 3 变体（每变体恰 1 条变换），全绿；
   **skip 允许量 ≤10%（A-8）**——|ts|<3 重 roll 后仍 skip 的程序占比超限即验收不过；
   batch seed 派生公式 `seed_i = int(sha256("42:" + i) 前8字节)` 取正 int48 值（M-6）
2. 注入坏 T3 → runner 报 mismatch 并落盘 finding
3. 同失败 seed 重跑 → 签名去重生效，报告 skipped
4. 任一 finding 经 reduce.sh 后仍满足复现判据（类别+根因特征）
5. Tier B 接入验收：对深尾递归基准程序施加 T12 后两侧均正常结束（TCO 保持性检验；
   本项为"基准形态+单变换"，不违反每变体恰 1 条变换的铁律）；β 展开作用于
   闭包捕获程序 → 输出一致

### DELIV-5: typecheck 双向
1. 100 正例全部 accept 且运行无崩溃
2. 400 负例（4 类 reject 型变异 ×100）全部 reject 且 0 panic
3. 穷尽性变异 50 例：全部出现 non-exhaustive warning 且 accept
4. 打乱定义顺序 50 次 → accept/reject 结论翻转 = 0
5. 对照组通过率 100%

### P1-DELIV-8: GC 差分
1. `TA_GC_STRESS=1` 下 W-pure 200 程序：normal vs stress compare() 差异 = 0
2. 多重集 harness K=16,M=100：排序后逐行比对通过；
   人为在 scheduler 注入丢弃一条投递 → harness 报警

### 其余交付物的验收映射（M-11）
- DELIV-4（reduce）← DELIV-3#4（同一场景覆盖）
- DELIV-6（TA_GC_STRESS）← P1-DELIV-8#1（同一旋钮同一场景）
- DELIV-7（spec int48 章节）← 文档 review 通过即可，无运行时场景
- DELIV-9（fmt 接入快速环）← 快速环 fmt 幂等性扫描连续 3 次 CI 绿
  （扫描逻辑本身由 §9 工作流命令保证，等价于对语料全量执行 DELIV-2 式比对）
- DELIV-10（Makefile.inc/nightly 接线）← nightly 首次全绿
- DELIV-11（Tier C CPS）← 上线门槛即 §5.2 的语料自检；接入后 nightly 全绿

---

## 11. 边界条件与风险

| 风险 | 缓解 |
|------|------|
| ast-dump 与 parser 内部结构耦合 | 快照机制 + AGENTS.md parser-ast.ta 同步义务顺带覆盖；issue #67 边界已知悉：ast-dump 测的是工作区 lib 版本，与 bootstrap 产物可能不同代 |
| 生成器自身 bug → 假阳性淹没 | 正例对照组 + §5.6 自检 + 归约器降低人工复核成本 |
| 工具链在 CI 缺失 | Makefile 检测；fast=SKIP 留痕 exit 0，nightly=exit 1（§9） |
| 负数 `/` `%` 语义坑 | §5.0 钉死 + DELIV-2 场景 3 设卡 |
| TA_GC_STRESS 引入新 bug 污染测量 | 默认关闭、独立 PR、合入前跑全量套件 |
| codegen 非确定性击穿 fmt 幂等性 | DELIV-9 首项验收即验证前提（§6.3） |
| match 穷尽性 warning 噪声 | 生成器保证自带通配臂或证明穷尽（§5.0） ||