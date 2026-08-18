# AGENTS.md

## Branch Policy

- **禁止直接在 `main` 分支上提交。** 所有改动必须通过 feature branch + PR 合并。
- `main` 分支只接受通过 GitHub PR 合并（squash merge 或 merge commit），不接受 `git push origin main`。
- 本地 `main` 只用于跟踪 `origin/main`，保持 `git pull --ff-only` 更新。

## Focus & Triage

- **用户有明确要求做 A 时，不要发散去做 B / C。** 专注主线，一次只推进一个目标。
- 做 A 的过程中遇到的改进点——比如语言当前阶段不好用才踩到的点、不影响当前进度的 bug——**不要顺手修在主线上**，也不要停下主线展开。
- 处理方式：主线照常推进；把发现的问题**单独提交一个 PR 记录下来**（一条分支一个问题，PR 描述写清背景），既不丢失，也不打断当前进度。

## Layering Principle（分层原则）

新功能 / bug fix 优先落在最外层，能不动下层就不动下层。层级从外到内：**用户层 → 标准库层（lib/*.ta）→ 编译器层（typecheck / codegen / parser）→ VM 层（ta.h / src/vm.c / src/api.c）**。

1. **能在用户层实现的，就不要动标准库层。**
2. **应该放到标准库层的，就不要去 hack 编译器层** —— lib/*.ta 能实现的（例如 `not` 用 `pub fn not(x: bool) -> bool`），不碰编译器里的 builtin 承诺。
3. **能编译器层处理掉的，就不要加到 VM 层** —— 编译期能报错（如 `undefined function`）就不加 opcode / VM 指令。

教训：`not` 的修复最初 hack 到 VM 层（OP_NOT opcode 60 + typecheck builtin 承诺），正确方案是 lib/bool.ta 普通库函数 + 编译器撤回承诺。

## Build & Test

- Bootstrap compiler: `make bootstrap`（用 lib/bootstrap.tabc 编译 lib/driver.ta）
- Fixed point 验证: `make bootstrap` 两次后 `cmp` 产物必须 byte-identical
- 测试: `make test`（7 个 category，必须 0 failures）
- **提交 PR 之前必须 `make fmt`**（格式化 C/C++ 文件；`.ta` 文件不在 clang-format 范围，靠手写风格 + bootstrap fixed point 保证一致性）
- 生成的文件: `lib/bootstrap.tabc` 是编译产物，修改 `.ta` 源码后必须 `make bootstrap` 重新生成

## Compiler Tests

- `test/compiler/parser-ast.ta` — parser AST 文档测试：每种语法构造一一对应期望 AST（纯正例）。修改 parser/tokenizer 语法时必须同步更新该文件
- `test/compiler/` 下 `*-errors.ta` / `*-parse-errors.ta` 是负例（必须被编译器拒绝）
- 单独跑 compiler 测试: `test/run_compiler_tests.sh`
- 改 `lib/*.ta` 后必须 `make bootstrap` 再跑测试，否则测的是旧编译器
- **改语言语法（parser/tokenizer/新语法构造）必须同步更新 `docs/ta-language-spec.md`**，并补测试：`test/basic/*.ta` 运行时正例（非 `*-errors.ta` 都会被 `tinyactor run` 跑）、`parser-ast.ta` 正例、`*-errors.ta` / `*-parse-errors.ta` 负例。只加语法忘记维护文档/测试 = 不完整提交

## TA Language Gotchas（踩坑记录）

- **`pair?`/`symbol?`/`int?`/`string?`/`null?` 返回布尔值**——`if` 条件里裸用（`if pair?(x) {`），**不要写 `pair?(x) == 1`**（恒 false，静默走 else，typecheck 不拦截）。`is_op`/`is_pipeable_head` 等**自定义谓词返回 int 0/1**，才用 `== 1` 比较。→ issue #65
- **字符串拼接用 `str.concat`（恰好 2 参）**；`+` 只用于数字。`str.concat` 传 3 参会报 "cannot unify string with 'a -> 'b"
- **`import foo` 链接 `lib/foo.ta` 当前源码**，不是 bootstrap.tabc 内嵌实现——写 `import` 调试脚本前先确认 `lib/*.ta` 源码状态与 bootstrap 一致，否则测的是"两个不同版本的库"。→ issue #67
- **parser 错误信息只有 token 编号**（如 `unbalanced '}' (token 9730)`），无行/列/上下文。定位用脚本逐行数括号（字符串/注释内的括号要排除）。→ issue #66
- **未 commit 的改动不要 `git checkout <file>`**——会丢掉全部未提交修改（本分支 pipe 实现曾因此被整个抹掉）。用 `git stash` 或先 commit

## Debug Loop（bootstrap 成本）

- `make bootstrap` 单次约 50s-2min（driver.ta 全量编译），fixed point rebuild 约 47s
- 在 `lib/*.ta` 里加 `print` 调试必须重新 bootstrap 才生效——**一次尽量多打点**（纯字符串标记 + `str.from_int` 分开打，避免拼接失败吞输出）
- 长编译用 tmux（socket `-L agent-pipe`），进程消失 ≠ 结束：`make bootstrap` 的 tavm 可能仍在跑，轮询 `ps` 或 done 标记文件
