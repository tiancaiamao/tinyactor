# lispvm 主线 TODO

> 语义与现状见 README.md。顺序即优先级；每项完成后勾选并回填实测数字。

## 已落定（背景，不再动）

- [x] lisp 内核 v1：compile.ta + lispvm.c，7/7 全绿（fib/closure/list/quote/collatz/collatz1m/map）
- [x] 语义锁定：lisp1 抄 Gleam——顶层 def 走静态 fnid 表（GLOBAL/CLOS_ID 零堆分配），
      匿名 lambda 走闭包值，`(f x)` 统一 CALL 路径；无递归匿名 fn（Gleam 也没有）
- [x] 基线（collatz 1M，同负载 max steps=525）：
      tavm 8.6s / **lispvm 4.8s** / vm_demo_acc 2.9s（peephole 上限参照）
- [x] 支线：TA 主线 `symbol?` opcode **已合并**（#226，main @ 885b4e8）

## Phase 1 — 对接层（已完成：1.1 #227 / 1.2 + VM #228 / 1.3）

- [x] **1.1 `sexp.c` C 模块**（照 str/buf 模块接口）→ **已合并（#227）**
      `sexp.parse(text) → nil 结尾的顶层 forms 列表`（不是 `'end` 结尾——那是 TA 同构
      类型系统的 builder 脚手架，树异构后退休）；硬错误返回 -1，详情 `sexp.err()`
      （`__thread` 缓冲，worker 串行无竞态）。语法 v1：注释/quote 糖/int48/转义
      string/深度 10000。GC 门内安全已 TA_GC_STRESS=1 实证（27/27）；bootstrap.tabc
      byte-identical（C-only 证明）。落地后顺手删 compile.ta 的排除法 sym?。
- [x] **1.2 内部表示 ADT 化**（仅内部实现层）
      compile.ta 的**输入层保持 sexp 树**（pair/symbol/int，来自 sexp.parse）；
      内部实现层——指令树等中间表示——改 TA `pub type` ADT + match 解构，cons 链消失。
      **验证**：重写前后 7/7 `.bc` byte-identical（同一组手拼 prog，旧编译器产物为基线）；
      lispvm 7/7 输出一致（6765/85/15/hello/59542/525/(1 4 9)）。
      **期间发现并修复 VM bug**：深递归负载（编译器本身就是）下 actor arena 卡死——
      `proc_stack_reserve` 帧预留只在 heap 空 时允许 grow，heap 非空直接 fatal，
      而指令边界的 256B headroom 保证不了超深调用链。修复：预留被拒时走 `gc_collect`
      回退（与 `proc_stack_headroom` 同款纪律：OP_CALL 已 SP_PUBLISH、TA 栈即根集、
      gc_gate open），vm.c 侧 SP_PUBLISH 后重读被搬移的 closure_val。
      `make test` 221 PASS / 0 FAIL。
- [x] **1.3 测试文本化**
      main.ta 从手拼 builder 改为读 `.lisp` 文本；7 个测试程序迁移成真 `.lisp` 文件，
      端到端 `sexp.parse → compile.ta → .bc → lispvm` 保持 7/7 全绿。
      手搓 AST 出错整类问题（`'end` 泄漏那类）连根消失。
      **验证**：sexp 路径产物与 builder 路径 7/7 `.bc` byte-identical（同一编译器对
      同构树）；lispvm 7/7 输出一致；错误路径实测——parse 错误带行列号退出 1
      （`line 3, col 1: unbalanced '('`），缺文件报路径退出 1。`sa..sd` builder
      脚手架退役；compile.ta 零改动（`pair?` 收尾遍历天然兼容 nil 结尾）。

## Phase 2 — 模块化（v2 字节码）

- [x] **2.1 链接器**：`.bc` 的 GLOBAL 操作数 fnid → 符号名，loader 侧解析
      名字→fnid（.o 式链接，extern 函数 = 链接符号表补项）
      **验证**：7/7 输出一致（GLOBAL 符号重定位后 fib 递归自引用、map 匿名
      lambda 全走通）；重复 def 加载期 `duplicate global 'f'` exit 1；
      collatz1m 4.9s 无回归。
      实现：FnRec/FnW/FnBC 加 name 字段（def 名 / 0 匿名），IGlob 携带符号、
      flatten 入常量池（与 quote 条目去重共享），f 记录头 4→5 词；lispvm
      `resolve_globals` 走 op 宽度表定位 GLOBAL 重定位，重复/未定义加载期 fatal。
      MAKE_CLOSURE 保持文件局部 fnid（链接时整体重定基，多文件在 2.2 落地）。
      **记录**：lambda 体内未定义名走自由变量捕获、entry 无闭包环境时运行期
      "LOADF on non-closure"——2.1 之前的既有行为，等 2.2 extern 落地时一并
      改成编译期诊断。
      **教训**：TA 宽松代码构造器 arity 不检查——FnRec 加字段漏改一处构造点，
      3 参模式遇 2 参值静默 miss 返回 nil，一路静默到顶层 match 才现形。
- [x] **2.2 库机制 + 编译期未定义拦截**：预编译 .bc + 多文件链接；
      `null?/not` 从烧平 opcode 移到 lisp 层库（prelude.lisp → prelude.bc，
      `null?`=eq? nil、`not`=if），opcode 只留最小自举集
      （算术/比较/eq?/cons/car/cdr/pair?/symbol?，OP_NOT/OP_NULLP 已删）。
      **编译期拦截**：未声明且未 def 的名字 = `Fail("undefined: g")` 编译错误
      （Out + Fail 变体；def 捕获链的 dfvs 并入 entry fvs 统一判）——
      2.1 遗留的"运行期 LOADF on non-closure"连根拔掉。
      **extern**：`(extern a b ...)` 顶层声明形（Bind(name,-2)，TA import 的
      对应物），compile_ref 的 `fid != -1` 统一走 GLOBAL 符号引用；
      lispvm 多文件 unit 化（word/fn/const 三基址重定位 + 跨单元按名
      intern + link_units 名字注册/重定位），`lispvm prog.bc prelude.bc`。
      **验证**：7/7 输出一致（6765/85/15/hello/59542/525/(1 4 9)），
      collatz1m 4.87s 无回归；负例 5/5——编译期 undefined: g / nope，
      链接期缺 prelude / 跨单元 duplicate 'dup' / extern 悬空 'ghost'。
      实现：flatten 主动把 def 名入池（纯库单元无调用点也有名字，否则
      nameidx 恒 -1、extern 永远链不到——2.1 的名字入池只靠调用点，
      prelude 是第一个纯库单元才暴露）；main.ta 编译 prelude（8 输出），
      Fail 时带名字列表退出 1；drv.ta + bad_*.lisp 负例固化为资产。
      **教训**：TA 变体名全局共享——`Err` 是 lib/result.ta 的 Result 构造器，
      lisp 这边 `Err(msg)` 静默构造了 Result.Err，typecheck 不报（compile()
      无返回注解，if 两臂类型 Result vs Out 不查）、match 静默 miss，
      一路 nil 到驱动才现形 → 改名 Fail 绕开（typecheck 缺陷，issue 已记）。

## Phase 3 — 优化（解冻部分）

- [x] ~~3.1 常量融合 peephole~~ —— **明确先不做**（已议定；vm-demo 数据留档备查）
- [ ] **3.2 GC：重用 ta 的**（不重写）—— NaN-boxing 值表示本就照抄 ta.h，
      分配/回收接 tinyactor 现有 GC，lispvm 不自搞一套
- [ ] **3.3 reduction 抢占 / prof**：对接回调度语义时才需要

## 对接方式（已定：方案 A，用户拍板）

lispvm 慢慢长大，sexp 输入层对接；旧 tinyactor 冻结为前端/宿主，逐步接管。
迁移路径：Phase 1-2 → lispvm 进 tinyactor 进程内（C 模块加载 .bc，共享 allocator）
→ actors 移植 → 旧 .tabc 路径冻结→删除。

## Phase 4 — lispvm 长出运行时能力（工作量主体，逐项啃）

- [x] **4.1 C 模块/cfunc 机制（最小纵切）**：GLOBAL linker 除 Lisp def 外可解析
      VM 注册表函数名；CALL 对 Lisp fnid/cfunc id 分流，共享统一 CALL 入口，
      参数在栈帧中连续传递并检查 arity。当前首个 host cfunc 为 `(extern print)`，
            一个参数打印并原样返回；编译器和字节码无需新增 opcode。extern 名即
      CLOS_ID（fnid ≥ nfns），能作一等函数值传参，CALL 统一入口分流。
      **验证**：9 正例输出一致（新增 cfunc2 = `(map print ...)` 打印 1/2、
      返回 (1 2)）；负例 6/6——新增 `bad_cfunc_arity` 传 2 参 →
      `arity mismatch` exit 1。原有 fib=6765、collatz1m=525 无回归；
      `make test` 0 failures。此为内建注册表的纵切，尚未接入 tinyactor 的
      模块 ABI/动态模块加载。
- [x] **4.2 match**：纯编译期 desugar——`compile_match` 把 match 展成
      `(let (t scrut) (if (test1 t) (arm1 t) ... nil))`，模式判定只用既有的
      `eq?`/`pair?`/`car`/`cdr`。**无新 opcode、无新 Ins 变体、字节码格式不变、
      lispvm.c 零改动**（这是本项能便宜落地的关键）。
      模式：字面量（int/true/false/nil）、`_`、变量绑定、`(a b)` pair 模式
      （car 对 a、cdr 继续对 b，可嵌套）。pair 模式只解一个 pair，不支持
      `(a b c)` 三元列表——与 TA 规范一致（`cons(a,b)` 解一个 pair）。
      **验证**：`(20 14 99 nil 3)` 五种模式全对（字面量命中 / 变量兜底 /
      `_` / 无匹配→nil / 嵌套 pair）；原有 9 个正例无回归；负例 6/6；
      `make test` 0 failures。
      **顺带修掉一个 4.1 之前就存在的真 bug**：entry 的 `next_slot` 从 0 起，
      而槽 0 是帧的 fn 槽（`param_bindings` 从 1 编参数正是为此），于是顶层
      `let` 存进槽 0、覆盖调用已压栈的函数指针 → "call on non-function"。
      match 展开成 let，所以必踩；改 `Ctx([], 1, ...)` 即解。
      复现（与 match 无关）：`(lst (let (x 5) x) 9)`。
      **教训**：调试时先用**不含新特性**的最小用例复现——我一度以为是自己
      的 match 写错了，实际 `(lst (if 1 2 3) 9)` 之外的 let 用例同样炸，
      `git stash` 回 4.1 基线复现才定位到既有缺陷。
- [x] **4.3 actors**：spawn/send/recv——移植 ta scheduler/reduction 语义
      （重用不是重写；mailbox、yield、抢占点）
      解释器寄存器（sp/base/cbase/depth/rsp/pc/acc）从 C 局部变量搬进 per-proc
      `Regs` 结构，切 proc = 换 `self` 指针，`SAVE_REGS`/`YIELD` 两个宏包住
      保存与让出——**opcode 体一行没改**，这是本项能便宜落地的关键（同 4.2 的
      "不加 opcode" 思路，只是这次落在运行期而非编译期）。
      调度：轮转 + reduction 预算（`PROC_BUDGET 10000`）作为抢占点；
      mailbox 是 malloc 的 `Msg` 链（**放在 proc 栈外**，栈会随让出重入而变，
      消息不能压在上面）；`recv` 邮箱空则置 `blocked_pending` 挂起，由
      `CALL_COMMON` 统一存回寄存器交调度器。`spawn` 的闭包自由变量已烤进
      arena cell，arena 全局唯一（无 GC），跨 proc 共享同一个 Val 即可。
      死锁判定 = 所有 proc 都在阻塞；只打印主 proc 的返回值，单 proc 输出
      因此与旧行为逐字一致。
      **验证**：`actor.lisp` 跨进程消息——子 proc `recv` 拿 6、`+1` 得 7、
      `send` 回父 proc，父 proc 阻塞在 `recv` 被唤醒后取到 7（输出 `7 7`）；
      原有 10 个正例输出与基线**逐字一致**（fib=6765、closure=85、list=15、
      quote=hello、map=(1 4 9)、cfunc=42 42、cfunc2=1 2 (1 2)、
      match=(20 14 99 nil 3)、collatz=59542、collatz1m=525）；
      负例 6/6；`make test` 0 failures（44/44、35/35）。
      **顺带修掉两个 4.3 之前就存在的真 bug**（都用 `git stash` 回基线复现
      确认，非本次重构引入）：
      (1) *让出点位置反了*——抢占判在 `stack[sp++] = acc` **之后**，而
      `SAVE_REGS` 存下的 sp 已含末参、pc 却仍停在 CALL，重入再执行一次
      CALL 就把 acc **重复 push**，每让出一次栈顶漂一格，最终 `stack[nb]`
      取到垃圾 → "call on non-function"（fib 当场炸）。让出点必须在 flush
      **之前**：pc 仍指向本 CALL，寄存器里的 acc 原样存回，重入后 flush
      只发生一次。
      (2) *entry 帧多预置了一个 slot*——`main_proc` 建栈时 `stack[0]` 塞了
      占位 `CLOS_ID` 且 `sp = 1`，但 entry 自己的代码以 `GLOB`+`PUSH` 把
      被调函数压进槽 0（帧约定：槽 0 = fn）。预置值把那次 PUSH 顶到槽 1，
      随后的 `STORE 1` 正好覆盖它 → 同样 "call on non-function"。
      改 `sp = 0`、槽 0 留空即解。
      另有一个**同类第三例**（挂起 cfunc 的重入）在本阶段才暴露：recv 挂起时
      `pc` 也停在 CALL 上，重入会把 acc（recv 的占位 nil）再 flush 一次。
      用 `Regs.in_cfunc` 标记挂起重入，跳过这次 flush 即可（`nb = sp - n`
      本就仍指向 fn 槽，不做 `sp--` 补偿）。

## 支线（不占主线，记录在案）

- perf/vm-dispatch-demo 分支：stash@{0}（json.ta + vm.c wip）回该分支再 pop
- depth 从 AST 算（depthpass 双份逻辑）：等编译器变大后再动（已议，后置）