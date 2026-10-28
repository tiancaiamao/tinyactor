# AGENTS.md

代码可读性与功能模块化设计是维护这个项目最重要的点。**其它任何东西都可以为此让步**，无论是功能，性能，哪怕是修 bug。
如果一个 bug 极难触发且危害有限，为了修复该 bug 会引入特别高的复杂度，则 bug 都可以不修，
直到有更优雅的设计来简化问题 -- 追求的哲学是：太简单以至于写不出 bug 来。

“设计编程语言不应该靠堆砌一个又一个的特性，而是应当移除那些让额外特性显得不可或缺的缺陷与限制。”
维护软件也是这样的。

从第一性原理出发，不要盯着问题看怎么修，盯着 feature 看怎么加，而是如何找到根本性需求，
如何简化以至于问题从根源上避免

## Branch Policy

- **禁止直接在 `main` 分支上提交。** 所有改动必须通过 feature branch + PR 合并。
- `main` 分支只接受通过 GitHub PR 合并（squash merge 或 merge commit），不接受 `git push origin main`。
- 本地 `main` 只用于跟踪 `origin/main`，保持 `git pull --ff-only` 更新。

## Focus & Triage

- **用户有明确要求做 A 时，不要发散去做 B / C。** 专注主线，一次只推进一个目标。
- 做 A 的过程中遇到的改进点——比如语言当前阶段不好用才踩到的点、不影响当前进度的 bug——**不要顺手修在主线上**，也不要停下主线展开。
- 处理方式：主线照常推进；把发现的问题**单独提交一个 PR 记录下来**（一条分支一个问题，PR 描述写清背景），既不丢失，也不打断当前进度。

## Layering Principle（分层原则）

新功能 / bug fix 优先落在最外层，能不动下层就不动下层。层级从外到内：**用户层 → 标准库层（lib/*.ta）→ 编译器层（lib/bootstrap/*.ta：tokenizer / parser / typecheck / lower-ast / compile / driver）→ VM 层（ta.h / src/vm.c / src/api.c）**。

1. **能在用户层实现的，就不要动标准库层。**
2. **应该放到标准库层的，就不要去 hack 编译器层** —— lib/*.ta 能实现的（例如 `not` 用 `pub fn not(x: Bool) -> Bool`），不碰编译器里的 builtin 承诺。
3. **能编译器层处理掉的，就不要加到 VM 层** —— 编译期能报错（如 `undefined function`）就不加 opcode / VM 指令。

教训：`not` 的修复最初 hack 到 VM 层（OP_NOT opcode 60 + typecheck builtin 承诺），正确方案是 lib/bool.ta 普通库函数 + 编译器撤回承诺。

## Build & Test

- Bootstrap compiler: `make bootstrap`（用 lib/bootstrap.tabc 编译 lib/bootstrap/driver.ta）
- Fixed point 验证: `make bootstrap` 两次后 `cmp` 产物必须 byte-identical
- 测试: `make test`（11 个 category，必须 0 failures）
- **提交 PR 之前必须 `make fmt`**（格式化 C/C++ 文件；`.ta` 文件不在 clang-format 范围，靠手写风格 + bootstrap fixed point 保证一致性）
- 生成的文件: `lib/bootstrap.tabc`（自举种子，**入库**）是编译产物，修改 `.ta` 源码后必须 `make bootstrap` 重新生成并提交。同趟写出的 `lib/bootstrap/driver.kl`（.kl 快照）是**本地产物，不入 git**，仅供 G-C 测试与观察
- **合并冲突中 `lib/bootstrap.tabc` 不要手合**：两个分支都改过 `.ta` 源码时必冲突。正确做法：解完源码冲突后 `make bootstrap` 重新生成（连跑两遍产物一致 = fixed point），再 `git add` 完成合并。保留任何一边的旧产物会让 fixed-point gate 必然 mismatch（PR #115 合并 main 时踩过）

## Compiler Tests

- `test/compiler/parser-ast.ta` — parser AST 文档测试：每种语法构造一一对应期望 AST（纯正例）。修改 parser/tokenizer 语法时必须同步更新该文件
- `test/compiler/` 下 `*-errors.ta` / `*-parse-errors.ta` 是负例（必须被编译器拒绝）
- 单独跑 compiler 测试: `test/run_compiler_tests.sh`
- 改 `lib/*.ta` 后必须 `make bootstrap` 再跑测试，否则测的是旧编译器
- **改语言语法（parser/tokenizer/新语法构造）必须同步更新 `docs/ta-language-spec.md`**，并补测试：`test/basic/*.ta` 运行时正例（非 `*-errors.ta` 都会被 `tinyactor run` 跑）、`parser-ast.ta` 正例、`*-errors.ta` / `*-parse-errors.ta` 负例。只加语法忘记维护文档/测试 = 不完整提交

## C 代码规范

尽可能地避免 static 全局变量的使用，这样的代码写出来不是并发安全的！
反例是之前有个宏，为了少传参数，就额外引用了 static Proc* g_proc 来实现，愚蠢至极

有节制地使用宏，如果可以用 static inline 函数替代的，写法都选择后者

### C 模块不得有全局状态

**写 C 模块时不要引入模块级全局变量**（全局表、全局句柄池、全局缓存、
全局 mutex）——这跟 tinyactor 的并发模型相悖：

- actor 的状态属于 actor，跨 actor 只用 mailbox 传值。模块级全局表把
  「这份状态归谁」变成「谁来加锁」，于是库层一个小需求就逼出锁、句柄
  语义、二次释放一串东西（#395 的历史：sha256 句柄从 Bytes 改成全局
  槽表 + 全局锁，vs-go 基准的并行度直接塌成 1 核）。
- **状态是数据就做成值**：C 侧写成纯函数（输入状态，输出下一个状态），
  状态由 actor 一路传下去（genserver 传 State 的样子）。`lib/sha256.c`
  就是这么做的——状态是摘要的中间状态（一段字节），模块里没有一个全局
  变量，也没有锁。

确实需要**可变** C 资源（buffer / tls / process / sqlite…）时，按下面顺序
选，越靠前越好。这是 Erlang/OTP 的排序：值 → 单 owner + 消息 → 共享。

1. **值化**：能写成纯函数就写成纯函数（上面那条）。
2. **单 owner actor + mailbox**（Erlang 的 port / gen_server 做法）：资源归
   一个 actor 所有，其它 actor 不碰资源，只发消息请 owner 代做；C 侧记录
   owner 的 pid，非 owner 调用直接报错。访问因此天然串行，不需要锁。
3. **真共享**：锁必须**按资源**分（每个资源一把、随资源生灭），不是全局
   一把；锁只护资源自身的不变量，**不跨昂贵操作**（不跨 syscall / IO /
   可能触发 GC 的分配）。
4. **`Resource` 不透明值**（issue #401 落地）：可变 C 资源（buffer / process /
   tls …）的标准形态——C 侧 `ta_resource_new(ptr, dtor)` 得到堆外 payload，
   每持有者一份堆内 header；值随 send/spawn 移交归属；`close()` 确定性释放，
   GC / actor 死亡 / VM teardown 三重兜底（忘记 close 不泄漏）。dtor 约束
   （不分配 TA 值、不重入 VM、不触发收集）见 docs/c-module.md §5。

> `lib/buffer.c` / `lib/process.c` / `lib/tls.c` 已迁移为第 4 档范例（设计：
> docs/c-resource-design.md）。原「裸 int 句柄 + 槽表 + generation」将就方案
> 与「语言层缺口 TODO」已随 #401 退休。


## TA Language Gotchas（踩坑记录）

- **`pair?`/`symbol?`/`int?`/`string?`/`null?` 返回布尔值**——`if` 条件里裸用（`if pair?(x) {`），**不要写 `pair?(x) == 1`**（恒 false，静默走 else，typecheck 不拦截）。`is_op`/`is_pipeable_head` 等**自定义谓词返回 Int 0/1**，才用 `== 1` 比较。→ issue #65
- **`car`/`cdr` 只接受 Pair，`Nil` 不是 Pair**——`car(nil)`/`cdr(nil)` 曾静默返回 nil，
  现在可证明为 Nil 的实参编译期报 E0001，运行期才出现的 nil 以 `cartype`/`cdrtype` 隔离死亡
  （#236）。遍历先判 `null?`/`pair?`；取第 k 个元素用 nil 安全的 `list_ref`/`list.nth`，
  不要写 `car(cdr(cdr(x)))`。→ issue #236
- **字符串拼接用 `str.concat`（恰好 2 参）**；`+` 只用于数字。`str.concat` 传 3 参会报 "cannot unify String with 'a -> 'b"
- **`import foo` 链接 `lib/foo.ta` 当前源码**，不是 bootstrap.tabc 内嵌实现——写 `import` 调试脚本前先确认 `lib/*.ta` 源码状态与 bootstrap 一致，否则测的是"两个不同版本的库"。→ issue #67
- **parser 错误信息只有 token 编号**（如 `unbalanced '}' (token 9730)`），无行/列/上下文。定位用脚本逐行数括号（字符串/注释内的括号要排除）。→ issue #66
- **未 commit 的改动不要 `git checkout <file>`**——会丢掉全部未提交修改（本分支 pipe 实现曾因此被整个抹掉）。用 `git stash` 或先 commit

## Debug Loop（bootstrap 成本）

- `make bootstrap` 单次约 50s-2min（driver.ta 全量编译），fixed point rebuild 约 47s
- 在 `lib/*.ta` 里加 `print` 调试必须重新 bootstrap 才生效——**一次尽量多打点**（纯字符串标记 + `str.from_int` 分开打，避免拼接失败吞输出）
- 长编译用 tmux（socket `-L agent-pipe`），进程消失 ≠ 结束：`make bootstrap` 的 tavm 可能仍在跑，轮询 `ps` 或 done 标记文件
