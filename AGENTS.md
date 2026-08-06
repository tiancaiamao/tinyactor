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