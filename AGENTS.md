# AGENTS.md

## Branch Policy

- **禁止直接在 `main` 分支上提交。** 所有改动必须通过 feature branch + PR 合并。
- `main` 分支只接受通过 GitHub PR 合并（squash merge 或 merge commit），不接受 `git push origin main`。
- 本地 `main` 只用于跟踪 `origin/main`，保持 `git pull --ff-only` 更新。

## Build & Test

- Bootstrap compiler: `make bootstrap`（用 lib/bootstrap.tabc 编译 lib/driver.ta）
- Fixed point 验证: `make bootstrap` 两次后 `cmp` 产物必须 byte-identical
- 测试: `make test`（7 个 category，必须 0 failures）
- 生成的文件: `lib/bootstrap.tabc` 是编译产物，修改 `.ta` 源码后必须 `make bootstrap` 重新生成

## Compiler Tests

- `test/compiler/parser-ast.ta` — parser AST 文档测试：每种语法构造一一对应期望 AST（148 cases，纯正例）。修改 parser/tokenizer 语法时必须同步更新该文件
- `test/compiler/` 下 `*-errors.ta` / `*-parse-errors.ta` 是负例（必须被编译器拒绝）
- 单独跑 compiler 测试: `test/run_compiler_tests.sh`
- 改 `lib/*.ta` 后必须 `make bootstrap` 再跑测试，否则测的是旧编译器