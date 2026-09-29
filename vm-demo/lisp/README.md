# lisp — 最小 Lisp 内核：TA 编译器 + C 字节码解释器

一个独立于 TA 现有 bytecode 的新实现（不读不写 `.tabc`）。如果性能足够好，
后续从这里的 lisp 内部表示开始替换旧 TA 实现，因此：

- **值表示 = tinyactor NaN-boxing 原样**（tag 表、int48 sign-extend、48bit arena
  索引全部照抄 ta.h / ta_inline.h）。替换期两个世界可以互通。
- **字节码/调用协议全新**，只服务这个 lisp 内核。

## 管线

```
foo.lisp(TA sexp 字面量) → compile.ta（纯函数编译器）→ foo.bc(文本字节码)
                                                        → lispvm.c 解释执行
```

- `compile.ta` — Lisp → 字节码。输入是 TA 数据（pair/symbol/int），输出
  `[fns, consts, code]`。无副作用。
- `main.ta` — 驱动：读 `.lisp` 文本（`file.read`），`sexp.parse` 成树，调 compile，
  用 `buf` C 模块写出 `.bc` 文件。parse/读文件失败带行列号退出 1。
- `lispvm.c` — 独立 C 解释器：读 `.bc`，跑，打印末表达式值。
- host cfunc 通过 VM 内建注册表按符号名链接：`(extern print)` 可调用
  `print`，签名固定为一参；CALL 参数和返回值均使用同一套 NaN-boxed `Val`。
  extern 名编译成 GLOBAL（CLOS_ID，fnid ≥ nfns），因此能作一等函数值传参
  （见 `cfunc2.lisp`：`(map print ...)`）。当前只用于验证 VM ↔ C 边界，
  模块动态加载及 str/buf/sexp 能力尚未接入。
- `match` 是纯编译期语法糖（4.2），展开成 `let` + 嵌套 `if` + `eq?`/`pair?`/
  `car`/`cdr`：无新 opcode、无新 `Ins` 变体、字节码格式不变。模式为
  字面量（int/true/false/nil）、`_`、变量绑定、`(a b)` pair 模式（car 对 a、
  cdr 继续对 b，可嵌套）。pair 模式是二元的，不支持 `(a b c)` 三元列表模式
  —— 与 TA 规范一致（`cons(a,b)` 解一个 pair，列表模式是它的语法糖）。
- **actors**（4.3）：`(extern spawn send recv self)` 四个 host cfunc，走 4.1
  已有的内建注册表，**无新 opcode、字节码格式不变**。解释器寄存器搬进
  per-proc `Regs`，切 proc = 换 `self` 指针，`SAVE_REGS`/`YIELD` 包住
  保存与让出（opcode 体不改）。轮转调度 + reduction 预算 `PROC_BUDGET 10000`
  作为抢占点；`recv` 邮箱空则挂起，消息到达后被唤醒；死锁 = 所有 proc 都在
  阻塞。mailbox 是 malloc 的 `Msg` 链，**必须在 proc 栈外**——栈会随让出
  重入而变，消息压在栈上会被后续求值覆盖。闭包自由变量已烤进 arena cell，
  arena 全局唯一（无 GC），所以跨 proc 共享同一个 `Val` 即可，无需复制。

## 一栈模型下的多 proc（4.3）

每 proc 一份 `Regs`（sp/base/cbase/depth/rsp/pc/acc）与自己的栈区间，
但**代码区全局共享**。换 proc 只是换 `self` 指针 + 该 proc 的栈基址。

让出点放在 `CALL_COMMON` 里、**在 `stack[sp++] = acc` 之前**——这是本设计
最容易踩的坑：`pc` 停在 CALL 上、acc 还在寄存器里，重入后 flush 恰好发生
一次；若让出点在 flush 之后，存下的 sp 已含末参而 pc 仍指 CALL，重入会把
acc **再 push 一次**，每让出一次栈顶漂一格，最终 `nb = sp - n` 取到垃圾
（"call on non-function"）。挂起 cfunc（`recv` 阻塞）同理，需 `Regs.in_cfunc`
标记重入以跳过 flush。

## 已知缺陷（独立分支/PR 修，不在主线）

- **`let` 槽与求值栈重叠**：`compile.ta` 的 `depthpass` 把 `IStore` 记作
  `d+1`（即 STORE 应推进 sp），但 `lispvm.c` 的 `op_store` 只写
  `stack[base+slot]`、**不推进 sp**。于是局部变量与求值栈共用
  `base+1..`，后续 PUSH 会覆盖活着的局部。已在基线复现（与 actors 无关）：
  `(lambda (n) (let (x n) (print (let (y 2) (+ x y)))))` → `arith on non-int`。
  这同时限制了 4.3：**闭包捕获的父帧局部（如 `(let (kid (spawn ...)))`）**
  不能在 actor 测试里使用。注意**不能**简单让 `op_store` 推进
  `sp = base+slot+1`——`CALL n` 的 `base = sp-n` 依赖 sp 与实参位置对齐，
  改了就破坏调用协议（fib/match/actor 全炸）。正解是修 `compile.ta` 的
  槽分配，让局部从求值栈之上起算。


## 值表示（照抄 tinyactor）

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
  flatten 主动入池（纯库单元文件内无调用也有名字，extern 才链得到）
- `entry` = 文件内绝对词下标；代码紧跟各自的 5 词头
- 常量节：`0 <val>`(int) | `1`(nil) | `2`(true) | `3`(false) |
  `4 <len> <bytes>`(symbol)，× nconsts

lisp symbol 编译期 = TA symbol（相等性即 ==）；常量池写名字（
`str.sym_to_str` 取名），C 侧按池序 intern（跨单元按名去重）。

**链接（2.1/2.2）**：每个 `.bc` 是一个编译单元，`lispvm prog.bc lib1.bc ...`
顺序加载（第一个文件 = prog，fn 0 = entry），统一重定位：CONST /
MAKE_CLOSURE 按单元基址平移；GLOBAL 操作数是（单元局部的）常量池符号
下标，按名字跨单元解析为全局 fnid 并原地回写（.o 式）。重复 def /
未定义引用（extern 补不到项）链接期报错 exit 1，不等运行。

**编译期未定义拦截（2.2）**：未声明且未 def 的名字 = 编译错误
（`undefined: g, nope`）——名字错误在前端暴露，不留给运行期 LOADF。
跨单元引用须显式声明 `(extern a b ...)`（TA `import` 的对应物），
链接期由其它单元的 def 补项。

## 语言（内核 v1）

特殊形：`quote if begin lambda let def(顶层) extern(顶层声明)`；值：
nil true false 整数 symbol；原生（烧平 opcode，最小自举集）：
`+ - * / modulo = < <= > >= eq? cons car cdr pair? symbol?`。
库函数（prelude.lisp → prelude.bc，可推导的不烧 opcode）：
`null?`(=eq? nil) `not`(=if)——引用侧 `(extern null? not)` 声明。
无 set——纯语言。`quote` 仅用于生成符号常量（'a =>
TAG_SYM）；quoted list `'(a b c)` 不支持，链表用 `(cons ...)` 显式构造
——对齐 TA 的 `[ ]` 哲学：数据 reader 属于宿主，内核编译器不长出
第二套数据字面量。

局限（v1）：只支持函数定义——`(def x (add 5))` 这类右值非 lambda 的
值定义不实现，用 `((add 5) 37)` 内联部分应用表达同样语义。

## 测试（10 正例 + 1 负例 + prelude，端到端）

```sh
./tinyactor run vm-demo/lisp/main.ta          # 编译 13 个 .lisp → .bc（打印 13 = 全部成功）
vm-demo/lisp/lispvm vm-demo/lisp/fib.bc vm-demo/lisp/prelude.bc     # => 6765（递归 + 深度调用）
vm-demo/lisp/lispvm vm-demo/lisp/closure.bc vm-demo/lisp/prelude.bc # => 85  （闭包捕获 + CLOS）
vm-demo/lisp/lispvm vm-demo/lisp/list.bc vm-demo/lisp/prelude.bc    # => 15  （TCALL + 库函数 null?）
vm-demo/lisp/lispvm vm-demo/lisp/quote.bc vm-demo/lisp/prelude.bc   # => hello（symbol 常量 + CAR）
vm-demo/lisp/lispvm vm-demo/lisp/collatz.bc vm-demo/lisp/prelude.bc # => 59542（纯 TCALL 循环基准）
vm-demo/lisp/lispvm vm-demo/lisp/map.bc vm-demo/lisp/prelude.bc     # => (1 4 9)（lisp1：lambda 作值传参）
vm-demo/lisp/lispvm vm-demo/lisp/cfunc.bc vm-demo/lisp/prelude.bc    # => 输出 42，返回值 42
vm-demo/lisp/lispvm vm-demo/lisp/cfunc2.bc vm-demo/lisp/prelude.bc   # => 输出 1 / 2，返回 (1 2)（cfunc 作值传参）
vm-demo/lisp/lispvm vm-demo/lisp/match.bc vm-demo/lisp/prelude.bc    # => (20 14 99 nil 3)（match 五种模式）
```

负例（编译期/链接期拦截，均 exit 1 或 Compile-Error）：

```sh
./tinyactor run vm-demo/lisp/drv.ta     # bad_undef→undefined: g / bad_undef2→undefined: nope（编译期）
vm-demo/lisp/lispvm vm-demo/lisp/list.bc                       # 缺 prelude → undefined global 'null?'
vm-demo/lisp/lispvm vm-demo/lisp/bad_dup1.bc vm-demo/lisp/bad_dup2.bc  # → duplicate global 'dup'
vm-demo/lisp/lispvm vm-demo/lisp/bad_ghost.bc vm-demo/lisp/prelude.bc  # extern 悬空 → undefined global 'ghost'
vm-demo/lisp/lispvm vm-demo/lisp/bad_cfunc_arity.bc vm-demo/lisp/prelude.bc  # print 传 2 参 → arity mismatch
```

collatz 基准（1..1000 步数求和，~6 万次 TCALL）：lispvm ≈ 3.4 ms，
同一负载跑在现有 tinyactor VM 上 ≈ 87 ms（含驱动每次重编 .ta），
约 26×。数字含进程启动，量级结论即可。

1M 基准（collatz1m.bc，与 vm-demo/collatz.ta 同负载：1..999999 取 max
steps => 525）：lispvm ≈ 4.8 s；tavm（原版）≈ 8.6 s；极致优化的
vm_demo_acc.c ≈ 2.9 s。lispvm 落在中间：无 GC/调度负担，但尚无
vm_demo_acc 的常量融合 peephole（ADDC/EQC/DIV2→shift）——差距来源
明确，属后续优化空间。