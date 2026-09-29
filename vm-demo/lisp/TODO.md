# lispvm 主线 TODO

> 语义与现状见 README.md。顺序即优先级；每项完成后勾选并回填实测数字。

## ⚠️ 2026-09 重定向：Phase 4 的方向错了，已回退

**lispvm 不是要替换 TA 的候选实现，是「换一套 VM 让 TA 变快」的验证台。**
真正的目标：给 TA 上层（tokenizer/parser/typecheck）换一个更快的 VM，拿到收益后
回灌 `src/vm.c`。唯一变的是 VM 这一层；**GC / 堆 / 调度 / C 模块 / 多单元链接
全部继承 TA，不重做**——重做一份对不上。分层与接缝详见 README「定位」。

Phase 4 的三个 Phase 里，**4.1（cfunc）和 4.3（actors）都是走偏了**：它们在
lispvm 里重写了 TA 已有的能力（597+82 行），却对「四条 VM 优化」一条都没验证。
4.3 甚至选错了寄存器纪律（每步 `SAVE_REGS` 写回，而非 TA 的 C 局部 + 退出点
写回），为此付出三个 bug 的代价。**均已回退**，只留 4.2（match 纯编译期 desugar）。

保留的真正资产：collatz 1M **4.9 s vs tavm 8.6 s**，且剥到 669 行后仍是 4.9 s
——收益来自 VM 核心本身。

**下一步**：让 `compile.ta` 直接吃 TA `driver.ta` typecheck 之后的那个 `ast`
（对接协议），并把 `match` 之类宏移到 codegen 之前展开以压缩 ast 词汇表。

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

> **注：2.2 的多单元链接 / extern / prelude 库单元已在重定向中回退**（模块系统继承
> TA）。下面保留为过程记录。留下的只有 2.1 的一半：单文件内 `GLOBAL` 按名解析
> （`link_unit()`）。两条「教训」仍然有效。

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
- [x] ~~**3.2 GC**~~ —— **删除**：GC 继承 TA，lispvm 不自搞一套
      （NaN-boxing 值表示本就照抄 ta.h；分配/回收接 TA 现有 GC）
- [x] ~~**3.3 reduction 抢占 / prof**~~ —— **删除**：reduction 抢占继承 TA 的
      `src/scheduler.c`；profiling 属 TA `src/vm.c` 侧，不在 lispvm 验证

## 对接方式（~~方案 A：lispvm 接管、旧 .tabc 路径删除~~ → 已否决）

**方案 A 作废**：lispvm 慢慢长大接管一切、旧 tinyactor 冻结后删除。否决理由——
TA 复杂度下改不动编译器 + VM 架构，且「两个后端」不可行，必须只有一层。**不存在
「新后端接管、旧后端留着」。**

**现行方案（唯一变 VM 这一层）**：

```
TA 上层（保留不动）tokenizer → parser → typecheck → ast(sexp)
                                                  │
                                        ★ 对接协议 = 这个 sexp ★
                                    ↓                        ↓
                    旧:codegen.ta → .tabc → src/vm.c   （自举链,不能碰）
                    新:lisp 编译器 → .bc  → lispvm.c     （唯一变的一层）
                                                  ↓
                                GC / 堆 / 调度 / C 模块 / arena ← 全部继承 TA
```

- 旧 `.tabc` 路径是**自举输入**（`ta.h:495` 明写「新 opcode 追加在末尾、绝不能
  重编号」），碰它就断自举链。所以目标形态是**同一个 TA 编译产物，由不同的 VM
  解释**，不是换一个编译器。
- 迁移路径：lispvm 核心先剥到最小（已完成）→ `compile.ta` 改吃 TA 的 ast →
  `match` 之类宏移到 codegen 之前展开，压缩 ast 词汇表 → arena 换成 TA 的
  `proc_heap_alloc`（栈是 GC 全部根集合，`ta.h:737`），从而能进 `src/`。

## Phase 4 — lispvm 长出运行时能力（~~4.1 / 4.3 整条作废~~）

**结论：这三项验证的是「lispvm 能自造一套 TA 已有的能力」，不是「VM 更快」。**
唯一留下的 4.2（match 纯编译期 desugar）证明的是「不加 opcode 也能长出语言特性」，
方法论可留；4.1/4.3 连同它们那 679 行一并删除。回退后 `lispvm.c` 1013 → 669 行，
**collatz 1M 仍 4.85-4.97 s**（基线 4.8 s）——收益本来就在核心里。

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
- [ ] ~~**4.3 actors**（597+82 行）~~ —— **已回退删除**：actor 调度、mailbox、
      抢占、per-proc 寄存器结构全是 TA `src/scheduler.c` 已有能力，重做对不上。
      它还选错了寄存器纪律（每步 `SAVE_REGS` 写回，而非 TA 的 C 局部 + 退出点写回），
      为此付出三个 bug 的代价。**回退掉的代码见 `git show 14942a1`**，三个 bug 的
      根因分析（让出点在 flush 之后 → acc 重复 push；entry 帧多预置槽 0 →
      覆盖 fn 指针；挂起 cfunc 重入 → 再 flush 一次）值得留给将来真正动
      `src/vm.c` 时参考。

## 支线（不占主线，记录在案）

- perf/vm-dispatch-demo 分支：stash@{0}（json.ta + vm.c wip）回该分支再 pop
- depth 从 AST 算（depthpass 双份逻辑）：等编译器变大后再动（已议，后置）