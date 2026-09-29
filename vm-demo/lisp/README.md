# lisp — 最小 Lisp 内核：lisp 编译器 + C 字节码解释器（TA 新 VM 的验证台）

- **值表示 = tinyactor NaN-boxing 原样**（tag 表、int48 sign-extend 全部照抄
  `ta.h` / `ta_inline.h`）。只有**编码**通用；内存所有权、GC 可见性、并发假设
  都**不**通用——那部分一律继承 TA，不在这里重做。
- **字节码/调用协议是新的**，只服务这个 VM 层。

## 定位：VM 层的验证台，不是替代实现（2026-09 重定向）

**唯一目标：让 TA 的 VM 层变得更快。** 做法是给 TA 上层（tokenizer / parser /
typecheck）换一个**新的 VM 实现**，看能拿多少性能。lisp 内核就是那台新 VM。

```
TA 上层（全部保留,不动）
  tokenizer → parser → typecheck  ──►  ast（纯 sexp: symbol/pair/int/string）
                                              │
                                              ▼
                              ★ 对接协议 = 这个 sexp ★
                                    ↓                ↓
                        旧:codegen.ta → .tabc → src/vm.c   （自举链,不能碰）
                        新:lisp 编译器 → .bc  → lispvm.c     （唯一变的一层）
                                              │
                                              ▼
                        GC / 堆 / 调度 / C 模块 / arena  ← 全部继承 TA
```

**分层的硬边界**：

- **唯一变的是 VM 这一层。** lispvm 不做 GC、不做调度、不做 C 模块、不做多单元
  链接——这些全是 TA 已有的能力，重写一份对不上。lispvm 只负责「字节码 + 解释」。
- **对接协议是 sexp。** TA 的 `codegen.ta` 已经在吃一个纯 symbol/pair 的树
  （`head == 'lambda`、`sym == '+`），那就是既有接缝。lisp 编译器最终直接吃
  `driver.ta` 里 typecheck 之后的那个 `ast`，而不是自己 parse `.lisp` 文本。
  TA 侧那个 ast 同样可以改：`match` 之类的宏在**进 codegen 之前**就展开，
  让约定的 ast 词汇表更小、更容易对接。
- **不做「两个后端」。** 旧 `.tabc` 路径是自举输入（`ta.h:495` 明写「新 opcode
  必须追加在末尾、绝不能重编号」），碰它就断自举链。所以不存在「新后端接管、
  旧后端留着」——只有**同一个 TA 编译产物，由不同的 VM 解释**。
- **性能收益已量化**：collatz 1M 同一负载，lispvm ≈ 4.9 s，tavm ≈ 8.6 s。

## 管线

```
foo.lisp（TA sexp 字面量）→ compile.ta（纯函数编译器）→ foo.bc（文本字节码）
                                                          → lispvm.c 解释执行
```

- `compile.ta` — Lisp → 字节码。输入是 TA 数据（pair/symbol/int），输出
  `[fns, consts, code]`。无副作用。**它就是将来替换 `codegen.ta` 的那一层**，
  所以它的输入是 sexp 而不是文本。
- `main.ta` — 驱动：读 `.lisp` 文本（`file.read`），`sexp.parse` 成树，调 compile，
  用 `buf` C 模块写出 `.bc`。**这是临时脚手架**——对接后由 TA 的 `driver.ta`
  喂 ast，这条路径消失。
- `lower-ast.ta` — **对接层**：TA parser 的 ast → lisp `compile` 能吃的 ast。
  已实证 TA ast 与 lisp 内核同形，只差 3 处（见下文「对接层」）。
- `lispvm.c` — 独立 C 解释器：读单个 `.bc`，跑，打印末表达式值。**当前形态是
  刻意剥到最小的**：单编译单元、无链接、无 cfunc、无 GC、无调度、无多 proc。
  留下的只有值表示 + 字节码 + 解释主循环。
- `match` 是纯编译期语法糖，展开成 `let` + 嵌套 `if` + `eq?`/`pair?`/`car`/
  `cdr`：无新 opcode、无新 `Ins` 变体、字节码格式不变。模式为字面量
  （int/true/false/nil）、`_`、变量绑定、`(a b)` pair 模式（可嵌套）。pair 模式
  是二元的，不支持 `(a b c)` 三元列表模式——与 TA 规范一致。
  **对接时这一层应该移到 codegen 之前**（TA 侧展开），让 ast 词汇表更小。

## 值表示（编码照抄 tinyactor）

```
TAG_INT    0xFFF1  (int48 sign-extend)
TAG_NIL    0xFFF2      TAG_TRUE 0xFFF3   TAG_FALSE 0xFFF4
TAG_SYM    0xFFF5  (48bit = 内部 id)
TAG_PAIR   0xFFF6  (48bit = arena cell 索引)
TAG_CLOS   0xFFF8  (48bit = arena cell 索引)
TAG_CLOS_ID 0xFFFB (48bit = fn_id，无自由变量免堆分配)
```

int 之外类型：高 16 位 tag，低 48 位负载。heap 对象 16B cell 对齐，
bump 分配，v1 无 GC。

## 一栈模型（无 locals 数组、无返回栈）

帧 = 栈上一段连续 slot：

```
base+0       fn（closure 或 CLOS_ID）
base+1..n    args
base+n+1..   let/spill 槽（STORE 分配，sp 只增不减，RET 一次收回到 base）
```

调用协议（corta/cora 式）：`(f a b)` → `[code f][PUSH][code a][PUSH]
[code b][CALL 3]`。CALL n：flush acc（最后一参）→ base=sp-n，fn@base，
参数 base+1..；RET：sp=base，acc=返回值。TCALL 同但原地换帧不增长。

acc 是唯一的"寄存器"：每个表达式开始时 acc 已死（值不占栈），
值表达式结束时 acc=其值。纯编译期纪律，运行期零跟踪。

## 指令集

```
CONST u16      acc = consts[k]（常量池下标）
LOAD u16      acc = sp[base+i]          STORE u16   slot=acc（分配一槽，depth+1）
LOADF u16     acc = clo.free[i]         PUSH                    *sp++ = acc
ADD/SUB/MUL/DIV/MOD      l=pop; acc=l op acc   (int)
LT/LE/GT/GE/EQ           l=pop; acc=bool
NOT/NULLP/PAIRP/SYMP     acc 原地变换
CONS          l=pop; acc=pair(l,acc)    CAR/CDR    acc 原地取
JIF t f       真→cbase+t 假→cbase+f     JUMP u16   pc=cbase+u（fn 内相对偏移）
CALL u16      n = 参数数+1              TCALL u16  尾调用：f+args 下移覆盖当前帧
RET           sp=base
MAKE_CLOSURE u16 u16   fnid nfree；free 值 flush 前已按序 PUSH 在栈顶
GLOBAL u16    acc = CLOS_ID(fnid)（顶层 def 的引用）
```

## 性能要点

- **每函数 max_depth 编译期算好**（depthpass 模拟 d：PUSH/STORE +1，
  二元 -1，CALL/TCALL d-n，MAKE_CLOSURE d-nfree+1）。运行期唯一栈检查
  在 CALL/TCALL：`base + callee.maxd ≤ stack_top`，其余指令零检查。
- C 侧 computed-goto 分发，sp/acc 全程 C 局部变量。
- CALL 命中 CLOS_ID（GLOBAL 引用的顶层函数）直接取 fnid，零堆解引用。
- **明确后置的优化（防过早优化）**：常量融合 peephole（ADDC/EQC/DIV2
  →移位）、GC、reduction 抢占、prof。内核 v1 的价值在简洁与通用，
  先做对接层（sexp parser → 替换真实 ta），语义稳定后再谈优化。

## 语义定位（已定）

**lisp1，抄 Gleam**：顶层 def + 匿名 lambda，唯一值空间。`(f x)` 统一
调用——f 是变量/参数即调用其绑定的函数值（TAG_CLOS / TAG_CLOS_ID 走
同一条 CALL 路径）；顶层 def 引用走静态 fnid 表（GLOBAL）。无动态
作用域；lambda 词法捕获自由变量（LOADF，即 Erlang fun / Gleam fn 的
捕获）。无 lisp2 function cell。

## 编译器结构（四遍，各自简单）

1. **codegen** — compile_expr 产指令树；if 是复合指令
   `[OP_IF, c, t, f]`；lambda 递归出 fnrec，自由变量按首遇序编号，
   体内 LOADF / 父侧按序 LOAD(槽)/LOADF + MAKE_CLOSURE。
2. **flatten** — 复合指令展开、label 回填、常量入池（== 去重）。
3. **depthpass** — 模拟 d 得每函数 maxd。
4. 驱动写文件（entry 偏移 = 各函数头+码长前缀和）。

## 字节码文件格式（v2 文本，全空白分隔整数流）

```
<nfns> <nconsts>
<nameidx> <entry> <nargs> <maxd> <codelen> <code...>   × nfns
```

- `nameidx` = 定义名的常量池下标（-1 = 匿名：entry / lambda）；def 名由
  flatten 主动入池（文件内无调用的顶层 def 也要有名字，link_unit 按名解析）
- `entry` = 文件内绝对词下标；代码紧跟各自的 5 词头
- 常量节：`0 <val>`(int) | `1`(nil) | `2`(true) | `3`(false) |
  `4 <len> <bytes>`(symbol)，× nconsts

lisp symbol 编译期 = TA symbol（相等性即 ==）；常量池写名字
（`str.sym_to_str` 取名），C 侧按池序 intern。

**单编译单元**：一个 `.bc` 就是全部。`link_unit()` 只做一件事——把 `GLOBAL`
操作数（常量池符号下标）按名解析成本文件的 `fn_id`；`CONST` / `MAKE_CLOSURE`
的操作数本就是本文件下标，无需平移。未定义的名字加载期报错 exit 1
（`undefined global`），不等运行。

**编译期未定义拦截**：未声明且未 def 的名字 = 编译错误（`undefined: g, nope`）
——名字错误在前端暴露，不留给运行期。

## 语言（内核 v1）

特殊形：`quote if begin lambda let def(顶层)`；值：nil true false 整数 symbol；
原生（烧平 opcode，最小自举集）：`+ - * / modulo = < <= > >= eq? cons car cdr
pair? symbol?`。无 `extern`——**跨单元链接与模块系统继承 TA，lispvm 不重做**，
单编译单元内顶层 `def` 即全部符号。`null?` 这类可从 `eq?`/`if` 推导的一行函数
在用到的地方本地 `def`。
无 set——纯语言。`quote` 仅用于生成符号常量（'a => TAG_SYM）；quoted list
`'(a b c)` 不支持，链表用 `(cons ...)` 显式构造——对齐 TA 的 `[ ]` 哲学：
数据 reader 属于宿主，内核编译器不长出第二套数据字面量。

局限（v1）：只支持函数定义——`(def x (add 5))` 这类右值非 lambda 的
值定义不实现，用 `((add 5) 37)` 内联部分应用表达同样语义。

## 测试（8 正例 + 2 负例，端到端）

```sh
./tinyactor run vm-demo/lisp/main.ta     # 编译 8 个 .lisp → .bc（打印 8 = 全部成功）
vm-demo/lisp/lispvm vm-demo/lisp/fib.bc         # => 6765（递归 + 深度调用）
vm-demo/lisp/lispvm vm-demo/lisp/closure.bc     # => 85  （闭包捕获 + CLOS）
vm-demo/lisp/lispvm vm-demo/lisp/list.bc        # => 15  （TCALL）
vm-demo/lisp/lispvm vm-demo/lisp/quote.bc       # => hello（symbol 常量 + CAR）
vm-demo/lisp/lispvm vm-demo/lisp/map.bc         # => (1 4 9)（lisp1：lambda 作值传参）
vm-demo/lisp/lispvm vm-demo/lisp/match.bc       # => (20 14 99 nil 3)（match 五种模式）
vm-demo/lisp/lispvm vm-demo/lisp/collatz.bc     # => 59542（纯 TCALL 循环）
vm-demo/lisp/lispvm vm-demo/lisp/collatz1m.bc   # => 525  （1M 基准）
```

负例（编译期拦截，Compile-Error）：

```sh
./tinyactor run vm-demo/lisp/drv.ta   # bad_undef → undefined: g / bad_undef2 → undefined: nope
```

链接期负例（跨单元重复 def、悬空 extern）随多单元链接一起剥离——那属于
模块系统，继承 TA。

对接层的测试（TA ast 进、lispvm 出）独立跑：

```sh
./vm-demo/lisp/run_bridge.sh     # 37 正例实跑 + 2 负例编译期拒绝 + 1 链接期负例
```

## cfunc / extern：lispvm 调 C 函数

lispvm 自己没有函数库，任何宿主能力都得显式声明。机制分三半，缺一不可：

**1. `extern` 是纯编译期声明。** `(extern print println)` 不是运行时调用，
编译器 `strip_externs` 把它从程序里剥掉，并按**首次出现顺序**把名字追加到 `.bc`
末尾的 extern 段。`(extern ...)` 本身不进字节码流。

**2. 未声明 = 链接期错。** `link_unit` 只绑定 extern 段里声明过的名字，声明表里
没有的一律 `link error: undefined extern: <name>`。所以 extern 拼错不会拖到运行时
才炸，也不会静默变成「调用一个不存在的函数」——`bridge.linkneg` 就是钉这一条的。
TA 侧同样把悬空 `extern` 剥掉，两边对称。

**3. native 值的 tag 是 `TAG_NATIVE 0xFFC1`**，负载是 `g_natives` 的下标，签名
`Val (*NativeFn)(Val *args, int nargs)`。被绑定的 extern 拿到 **fid = -2**，指令里
不再按名字找——`IGlob` 在任意深度都能解析它，所以调用点不需要知道它是不是全局。

```
(extern print)                 ; 声明：只影响链接，不产生代码
(print 42)                     ; 调用 → fid -2 → g_natives[0]
```

`-q` 关掉「打印 entry 值」那一句，只留程序自己的 `print` 输出。对拍 TA runtime
必须带 `-q`：`print` 不换行，entry 值会和最后一行输出粘在一起（`7nil`），
没法用 `tail -1` 切干净。

## prelude：让 `.lisp` 有 `null?` / `not`

lisp 内核刻意不抄一份标准库（库/模块机制继承 TA），但 `null?` / `not` 是写任何
非平凡 lisp 都要用的。放 `prelude.lisp`，在 `main.ta` 的 `build()` 和
`bridge_test` 里都用 `list.append(main.prelude(), forms)` 前置。

**prelude 是 `.lisp` 而不是 `.ta`**：lisp 数据是异构的，TA 的类型系统过不了。
代价是 `.lisp` 里不支持字符串（kind -1 载入即拒）——独立特性，不在本次范围。

## 性能基准

collatz 1M（`collatz1m.bc`，与 `vm-demo/collatz.ta` 同负载：1..999999 取 max
steps => 525）：

| 实现 | 耗时 |
|---|---|
| `tavm`（`src/vm.c`，原版 TA VM） | ≈ 8.6 s |
| **`lispvm`（剥到最小的核心）** | **≈ 4.9 s** |
| `vm_demo_acc.c`（含常量融合 peephole 的上限参照） | ≈ 2.9 s |

剥掉链接/cfunc/actors（1013 行 → 669 行）后重测仍是 4.85-4.97 s，说明收益来自
VM 核心本身，不是被剥掉的那些功能。lispvm 落在中间：无 GC/调度负担，但尚无
peephole（ADDC/EQC/DIV2→shift）——差距来源明确，属后续优化空间。

## 对接层：lower-ast.ta（已完成，接缝已实证）

`lower-ast.ta` 就是「让 lisp 编译器吃 driver.ta 的 ast」的那一层：
`TA parser 的 ast` → `lower_program` → `lisp compile.compile` 能吃的 ast。
**37 个正例端到端跑通**（TA 源码 → 编译 → lispvm 实跑 → 值相等）。

结论：**TA 的 ast 已经是标准扁平 sexp**（int / string / symbol / nil / 扁平
cons 列表），与 lisp 内核同形。差的只有 3 处，全在 `lower-ast.ta` 里抹平：

| TA ast | lisp 需要 | 为什么 |
|---|---|---|
| `(define (f a b) body)` | `(def f (lambda (a b) body))` | 名字是 `car(sig)`，参数是 `cdr(sig)`；body 可能是多语句 → `begin` |
| `(let x init body...)` | `(let (x init) body)` | TA 无多绑定 `let`，恒右嵌套 |
| `import` / `type` / `const` / `type-sig` / `external_fn` | 丢弃 | 这些是 TA 编译期概念，lisp 内核不需要（模块系统继承 TA） |

外加 3 个 TA 侧词形 lisp 内核没有，各一条降级规则：
`and` / `or` → 临时槽 + `if`（短路且返回操作数本身，不是 bool）；
`list` → 嵌套 `cons`；`match` / `pipe` / `&&` / `||` / `[1,2,3]`
**TA parser 已经展开完了**，lower-ast 原样透传，一行代码都不用写。

> 这就是「把 match 之类的宏在 codegen 之前展开、让约定 ast 更小」的收益：
> 展开点选在 TA parser（已经在做），lisp 侧零成本。`match` 只需留在 lisp
> `compile.ta` 里做纯编译期脱糖（无新 opcode），或者干脆也上提到 parser。

运行：

```sh
./vm-demo/lisp/run_bridge.sh
# === bridge: 37 passed, 0 failed
# === negative: 2 rejected, 0 wrongly accepted
# === link negative: 1 rejected, 0 wrongly accepted
# === quiet: 1 passed, 0 failed
```

正例覆盖 `define` / 多 define / 递归 / `let` 三种形态 / `begin` / 闭包 /
`quote` / `list` / `if-else` / `pipe` / `&&`/`||` / `import` 丢弃 / `match`，
外加 cfunc（`print` / `println` / 尾位置 native）、prelude（`null?` / `not`）、
以及入口约定（有 `def main` 自动追加 `(begin (main) nil)`；顶层特殊形式
`begin`/`if`/`let`/`quote` 走 `compile_form` 而非「当函数调用」）。
负例断言**不认识的东西必须在编译期被拒**（`undefined: g`、`undefined: spawn, g`），
而不是跑出错数——这是 lower-ast 不静默错降级的唯一保障。

## 语料覆盖：lispvm 能跑多少真 TA 代码

`run_corpus.sh` 拿 `test/basic` 里 73 个非 `-errors` 文件逐个过一遍真实管线
（tokenize → parse → typecheck → lower-ast → compile → lispvm 实跑），跟
`tinyactor run` 的 stdout 逐字节对拍：

| 指标 | 数量 / 73 |
|---|---|
| 编译通过（产出 `.bc`） | **29**（40%） |
| └ 语义与 TA runtime **完全一致** | **12** |
| └ 编译过但输出不一致 | 17 |
| 链接期拒绝 | 34 |
| 编译器不收敛（挂） | 10 |
| parse / tokenize / lower 失败 | 0 |

**34 个拒绝是同一类**，全是缺 C 模块 extern，不是语言层面的失败：
`str.concat`（2333 次提及）、`result.*`、`list.*`、`net.*`、`tcp.*`、`random.*`。
换句话说 **lisp 内核本身不认识库函数**，而 `test/basic` 大量在用库。

17 个不一致里 **12 个是 `lispvm: bad const kind`**——同一个根因：`.bc` 的常量段
没有字符串 kind，lisp 侧字符串一票否决。语料里 151 处裸字符串字面量、
约 75 处 `str.concat`、425 处 `print(ARG)`，**字符串是当前最大的单点阻塞**。
剩下 5 个是真语义差：2 个 `arith on non-int`、1 个 `call on non-function`、
2 个值不符（`111` / `1034`）——其中 `arith on non-int` 命中下面「已知缺陷」第 1 条。

优先级因此很明确，不需要猜：**先让 `.bc` 支持字符串常量**（一次改动解锁 ~12 个
文件），**再补库 extern**（解锁 ~34 个文件里的大部分）。两件都在 VM 层，不碰
编译器自举链。

## 下一步

1. **`.bc` 常量段加字符串 kind**（kind -1 已在 `main.ta` 里预留，lispvm 载入即拒）。
   解锁 12 个文件，是投入产出比最高的一步。
2. 补库 extern（`str.concat` / `str.from_int` / `list.*` / `result.*` …），
   走同一套 `(extern ...)` 声明，不给编译器加特例。
3. `compile.ta` 的入口约定已定（有 `def main` 追加 `(begin (main) nil)`，包在
   `begin` 里而不是裸 `(main)`——裸调用落尾位置会编成 `ITCall(1)`，lispvm 在
   最外层 entry frame 上会返回闭包本身而不调用它，表现为打印 `#<fn3>`）。
   下一步是把它接进 `driver.ta:1654` 那一行。
4. lispvm 的 arena 换成 TA 的 `proc_heap_alloc`（栈是 GC 全部根集合，
   `ta.h:737`），从而能进 `src/` 而非独立进程。这是「GC/堆/调度走 TA」的
   最后一环。
5. `compile.ta` 加调用点 arity 检查（现在不查，`fn f(a,b)` 被 `f(1)` 调用
   能编过，跑出别的值）。

## 已知缺陷（独立分支/PR 修，不在主线）

- **`let` 槽与求值栈重叠**：`compile.ta` 的 `depthpass` 把 `IStore` 记作 `d+1`
  （即 STORE 应推进 sp），但 `lispvm.c` 的 `op_store` 只写 `stack[base+slot]`、
  **不推进 sp**。于是局部变量与求值栈共用 `base+1..`，后续 PUSH 会覆盖活着的
  局部。**精确触发条件**（2026-09 实测，四个形状对比）：

  | 形状 | 结果 |
  |---|---|
  | `(let (y (+ x 1)) (* y 2))` | 22 ✓ |
  | `(let (y (g x)) (+ y 1))` | 111 ✓ |
  | `(let (y x) (+ y (g 1)))` | 111 ✓ |
  | `(let (y x) (let (z (g y)) (+ y z)))` | **`arith on non-int`** |

  即：外层局部还活着时，内层 `let` 的**初始化式里带一次调用**。调用压栈时
  把外层局部盖掉。注意**不能**简单让 `op_store` 推进 `sp = base+slot+1`
  ——`CALL n` 的 `base = sp-n` 依赖 sp 与实参位置对齐，改了会破坏调用协议
  （fib/match/actor 全炸）。正解是修 `compile.ta` 的槽分配，让局部从求值栈
  之上起算。

  该形状已登记为 `bridge.known` 里的 `letinit-call`，`run_bridge.sh` 每次跑都
  报 XFAIL；VM 修好后自动变 XPASS，届时移进 `cases()`。`lower-ast.ta` 的
  `and`/`or` 降级不受影响（临时槽绑的是已求值的表达式，调用落在 `if` 分支）。
