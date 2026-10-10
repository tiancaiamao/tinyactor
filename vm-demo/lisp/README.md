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
                        新:lisp 编译器 → .tabc  → lispvm.c     （唯一变的一层）
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
- **性能收益已量化**：collatz 1M 同一负载，lispvm（桥接 TA 运行时后）≈ 6.3 s，
  tavm ≈ 8.7 s。纯自建内核时期曾测得 4.9 s，接回 TA 堆/GC 是有意的取舍。

## 管线

```
foo.lisp（TA sexp 字面量）→ compile.ta（纯函数编译器）→ foo.tabc（文本字节码）
                                                          → lispvm.c 解释执行
```

- `compile.ta` — Lisp → 字节码。输入是 TA 数据（pair/symbol/int），输出
  `[fns, consts, code]`。无副作用。**它就是将来替换 `codegen.ta` 的那一层**，
  所以它的输入是 sexp 而不是文本。
- `main.ta` — 驱动：读 `.lisp` 文本（`file.read`），`sexp.parse` 成树，调 compile，
  用 `buf` C 模块写出 `.tabc`。**这是临时脚手架**——对接后由 TA 的 `driver.ta`
  喂 ast，这条路径消失。
- `lower-ast.ta` — **对接层**：TA parser 的 ast → lisp `compile` 能吃的 ast。
  已实证 TA ast 与 lisp 内核同形，只差 3 处（见下文「对接层」）。
- `lispvm.c` — 独立 C 解释器：读单个 `.tabc`，跑，打印末表达式值。**当前形态是
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

**单编译单元**：一个 `.tabc` 就是全部。`link_unit()` 只做一件事——把 `GLOBAL`
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
./tinyactor run lib/bootstrap/main.ta     # 编译 8 个 .lisp → .tabc（打印 8 = 全部成功）
vm-demo/lisp/lispvm vm-demo/lisp/fib.tabc         # => 6765（递归 + 深度调用）
vm-demo/lisp/lispvm vm-demo/lisp/closure.tabc     # => 85  （闭包捕获 + CLOS）
vm-demo/lisp/lispvm vm-demo/lisp/list.tabc        # => 15  （TCALL）
vm-demo/lisp/lispvm vm-demo/lisp/quote.tabc       # => hello（symbol 常量 + CAR）
vm-demo/lisp/lispvm vm-demo/lisp/map.tabc         # => (1 4 9)（lisp1：lambda 作值传参）
vm-demo/lisp/lispvm vm-demo/lisp/match.tabc       # => (20 14 99 nil 3)（match 五种模式）
vm-demo/lisp/lispvm vm-demo/lisp/collatz.tabc     # => 59542（纯 TCALL 循环）
vm-demo/lisp/lispvm vm-demo/lisp/collatz1m.tabc   # => 525  （1M 基准）
```

负例（编译期拦截，Compile-Error）：

```sh
./tinyactor run vm-demo/lisp/drv.ta   # bad_undef → undefined: g / bad_undef2 → parse error
```

对接层的测试（TA ast 进、lispvm 出）独立跑：

```sh
./vm-demo/lisp/run_bridge.sh     # 36 正例实跑 + 1 负例编译期拒绝
# === bridge: 36 passed, 0 failed
# === negative: 1 rejected, 0 wrongly accepted
# === quiet: 1 passed, 0 failed
```

## cfunc：按名调用宿主（Erlang 式，无声明）

lispvm 自己没有函数库，宿主能力**按名在运行期解析**，不需要任何声明：

**1. GLOBAL 不再链接期定死。** `link_unit` 只把「本单元 def 的名字」重定位成
fnid；其余 GLOBAL 保留符号，载荷改成 `-(vm 符号 id)-1`。CALL 期拿到负数才按名
走宿主：`vm_find_cfunc` → 带点名字 dlopen `lib/<mod>.<ext>` 自动加载（照抄
`CASE(OP_CCALL_NAME)` 的 miss 路径）→ 再 miss 则弹参压 nil（对齐 lisp 语义）。

**2. 代价：调用头的名字错误从编译期挪到运行期。** 编译器不再维护 VM 原生表
副本（旧 `(extern ...)` 机制必然漂移，已删）。值位置的裸名仍走自由变量捕获、
entry 层编译期拦（bridge 的 `undef` 负例钉这一条）。

**3. native 值的 tag 是 `TAG_NATIVE`**，载荷 = `vm->symbols` 下标（TA 自己的
符号表，无平行表）。cfunc 调用前后必须 `proc_gc_enter/leave`——实参指针只在
GC 闸门关闭时有效（同 src/vm.c 的调用约定）。被调方的名字解析成 TA 的 cfunc
（`vm_new` 注册的那批：`str.*` / `list.*` / `print` …）。

`print` 就是 TA 的 `print`：打印值 + 换行、返回 nil。TA 没有 `println`，写了
按名 miss 得 nil。`-q` 关掉「打印 entry 值」那一句。

## 库函数：prelude 只前置 `null?`

lisp 内核刻意不抄一份标准库（库/模块机制继承 TA），prelude 只提供 TA 真
builtin。`null?` 是 builtin，由 `main.ta` 的 `lib_src()` 返回 **lisp 源码
文本**，驱动侧前置：文本管线（`main.build` / bridge）直接 `str.concat`
进源码；AST 管线（corpus1）`sexp.parse` 回 forms 再 `list.append`——旧
prelude.lisp 的教训仍然成立：lisp 数据是异构的，TA 的类型系统过不了，
文本拼接不碰类型层。

`not` 曾和 `null?` 一起前置，但这与 TA 分层相悖（`not` 是 lib/bool.ta 的
库函数，须 import）：用户 `import bool` 展开后与 prelude 的 `def not`
撞出 duplicate global（#235）。现 prelude 不含 `not`，`import bool` 即得；
compile 的 collect_globals 对重名 def 编译期拒绝，双保险。

## 性能基准

collatz 1M（`collatz1m.tabc`，与 `vm-demo/collatz.ta` 同负载：1..999999 取 max
steps => 525）：

| 实现 | 耗时 |
|---|---|
| `tavm`（`src/vm.c`，原版 TA VM） | ≈ 8.7 s |
| **`lispvm`（桥接 TA 运行时，当前主路径）** | **≈ 6.3 s** |
| `lispvm`（历史：剥到最小的自建内核，4.9 s） | ≈ 4.9 s |
| `vm_demo_acc.c`（含常量融合 peephole 的上限参照） | ≈ 2.9 s |

桥接后慢于自建内核期（4.9 → 6.3 s）：值的产生走 TA 堆 + moving GC，换来的是
与宿主完全一致的语义（bootstrap fixed-point 已实证）。栈安全纪律是编译期的：
bc 每个函数带 maxd（帧内峰值），CALL/TCALL 按 base+maxd 一次预留整帧，
NEXT 零检查 —— 栈检查只发生在 SP 变化的指令上。

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

`run_corpus.sh` 拿 `test/basic` 里 74 个非 `-errors` 文件逐个过一遍真实管线
（tokenize → parse → typecheck → lower-ast → compile → lispvm 实跑），跟
`tinyactor run` 的 stdout 逐字节对拍（log 时间戳归一为 `[TS]`）：

| 指标 | 数量 / 74 |
|---|---|
| 编译通过（产出 `.tabc`） | **72**（97%） |
| └ 语义与 TA runtime **完全一致** | **70** |
| └ 编译过但输出不一致 | 2（`net-load` / `tls-lib`，负载/TLS 场景） |
| 编译器不收敛（挂） | 2（`http-lib` / `http-serve`） |
| 链接期拒绝 | 0 |
| parse / tokenize / lower 失败 | 0 |

剩余 4 个文件都在 net/TLS 家族，是 cfunc yield 重入与调度器在真实负载下
的深层问题（actor 集群 + 超时 + TLS 握手），不是语言/编译层缺陷：
`http-lib`、`http-serve` 挂（accept/handshake 等待链），`net-load` 丢消息
（collect 超时）、`tls-lib` 在 `tls.connect` 场景静默。另有一条已定位
未修的旁支：**未 import 的模块点调用会被编成同名 C cfunc**（如
`tls.connect` 直达 C `tls_connect` 而不是 lib/tls.ta 的 facade）——
expand_imports 只重写已 import 模块，属已知边界，见 git log
`28967c8` 之后的跟进项。

## 下一步

1. ~~`.tabc` 常量段加字符串 kind~~ **已做**：kind 5 = str len bytes，
   `const_words` / `parse_const` 两侧就位。
2. ~~补库 extern~~ **已做且更进一步**：extern 机制整个删除，宿主 cfunc 按名在
   CALL 期解析（Erlang 式），编译器零副本。
3. `compile.ta` 的入口约定已定（有 `def main` 追加 `(begin (main) nil)`，包在
   `begin` 里而不是裸 `(main)`——裸调用落尾位置会编成 `ITCall(1)`，lispvm 在
   最外层 entry frame 上会返回闭包本身而不调用它，表现为打印 `#<fn3>`）。
   下一步是把它接进 `driver.ta:1654` 那一行。
4. ~~lispvm 的 arena 换成 TA 的 `proc_heap_alloc`~~ **已做**：求值栈就是 TA 的
   Proc 栈，闭包/字符串/pair 全走 `proc_heap_alloc` + TA 堆，GC 根 = `p->sp`
   区间精确覆盖；lispvm 以 `make lispvm` 链接 `src/*.o`（去 tavm.o）。
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

  `lower-ast.ta` 的 `and`/`or` 降级不受影响（临时槽绑的是已求值的表达式，
  调用落在 `if` 分支）。

## 嵌回 TA：宿主面已打通（2026-09-30 实测；**本节方案已全部落地**）

> 状态：本节描述的改造已实现——`make lispvm` 把 `lispvm.c` 链到 `src/*.o`
> （去 tavm.o），值/堆/GC/符号表/打印/cfunc 全部走 TA 本体；extern、
> prelude.lisp、自造 print/println 已删。bridge 36 正例 + 负例 + quiet 全过。

目标不是"lispvm 跑得比 TA 快"，是**能接回去**。接回去 = 除了 VM opcode 集
和 `compile.ta`，其余全部用 TA 的本体。据此，本文件此前描述的多处实现都是
分叉，且其中一处重犯了 `not` 的教训：

- `prelude.lisp` 把 `(def not ...)` 前置进**每个编译单元**。而 `lib/bool.ta:5`
  就有 `pub fn not`。这与 AGENTS.md 记的教训同类（当年 `not` 被 hack 成
  OP_NOT opcode 60 + typecheck builtin 承诺），性质更坏：不用 import、无法覆盖。
- `print` / `println` 是 lispvm 自造的，且**语义与 TA 相反**：TA 的 `print`
  换行（`src/api.c:247`），lispvm 的不换行。此前代码里"TA 的 print 不换行"
  的注释是错的。凡是比较过输出正确性的结论都要重测。

### 1. 宿主面不需要改 TA 一个字

`ta.h` 可独立编译；把 `lispvm.c` 链到 `SRC` 除 `tavm.o` 外的全部对象上即可。
宿主序列照抄 `src/tavm.c:71-116`（`vm_new()` + 那份 `vm_register_*_module`
注册表）。实测在同一次运行里完成：

```
val_string(p, "hello from TA heap", 18) → TAG_STRING，text 正确
vm_find_cfunc(vm, "str.concat")        → 5
vm->cfuncs[5].fn(vm, parts, 2)        → "ab"   （TA 堆上的真字符串）
vm_intern_symbol(vm, "my-symbol")      → 45
```

这正是独立 lispvm 里 `str_concat` 会 segfault 的那条缝：宿主设好
`tls_current_proc`、堆是 TA 的，就通了。**TA 自己的 C 函数可以按名直接调用，
不需要任何适配代码**——lispvm 的 `Native` 表与 TA 的 `vm->cfuncs` 同构。

### 2. lispvm 的求值栈必须换成 TA 的 Proc 栈（已证明，非推测）

lispvm 的求值栈是私有 C 数组 `Val *stack`。TA 的 GC 扫 `p->sp`，**看不见
C 数组**。实测：64 个只存在 C 数组里的 Val，跨一次会分配的 `str.concat`
之后 **6 个被回收/覆盖（58/64 存活）**；同样 64 个 `proc_push` 到 `p->sp`
的，**64/64 存活**。

这解释了 `vm.c:1560` 那段注释为何要对 C 回调 `proc_gc_enter` 关 GC 门：
C 模块会在自己的堆上分配，而 args 在 C 局部变量里。lispvm 比那更糟——它是
整个解释器主循环，手里有几百个活 Val。**不能**对主循环套 `proc_gc_enter`
（那等于全程关门，即永不 GC）。

`ta.h` 无影子栈 / 根注册 API（已 grep 确认），所以没有更省的路：栈只能是
`p->sp`，用 `ta_inline.h` 里现成的 `proc_push`/`proc_pop`/`proc_peek`。

**推论**："lispvm 不做 GC"不是"以后复用 TA 的 GC"，而是当前结构上**无法**用
TA 的 GC。现在没 GC 是撞对了。

### 3. 因此的执行顺序（被依赖关系强制）

1. 宿主接线（上面已证，不改 TA）
2. 求值栈 → Proc 栈（真正的活；之后 GC 才看得见 lispvm 的活值）
3. payload/堆/符号表/打印器 → TA 本体。**payload 语义不同是硬阻塞**：
   TA 的 pair 负载是指针（`src/val.c` 的 `box_tag_payload(TAG_PAIR, (uint64_t)hp)`），
   lispvm 是 arena 下标。TA 的 `car`/`cdr` 拿到 lispvm 的 pair 会解下标当指针。
   13 处 `arena[val_payload(v)]` 机械替换为 `val_get_car`/`val_get_cdr`。
4. 删掉全部分叉：`prelude.lisp`、`TAG_NATIVE`/`g_natives`、`.tabc` extern 段、
   操作数里的负 native 下标、自造的 `print`/`println`、`bridge.linkneg`
   （TA 的语义是调用期按名解析 + dlopen 自动加载 + miss 给 nil，不是链接期硬错）。
