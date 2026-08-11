# TinyActor

轻量级 Actor 语言 + VM。函数式、类型安全，Erlang 风格的 Actor 并发跑在自举编译器与轻量字节码 VM 上，可嵌入 C。

## 一屏看懂

```ta
// ADT 定义消息的两种形状，match 按模式拆开处理
type Msg { Add(a, b); Mul(a, b) }

fn worker() {
  match recv() {
    Add(a, b) -> print(a + b)   // 收到 Add(20, 22) → 42
    Mul(a, b) -> print(a * b)
  }
}

fn main() {
  let pid = spawn(fn { worker() })  // 并发：启动一个 actor
  send(pid, Add(20, 22))            // 通信：消息就是 ADT 值
}
```

```console
$ ./tinyactor run hello.ta
42
```

## 特性

- **Actor 并发是一等公民** — `spawn` / `send` / `recv` 就是语法，写并发没有中间层；每个 actor 独立 GC，无全局 STW
- **类型安全** — Hindley-Milner 类型推断 + 泛型 ADT + 模式匹配穷尽性检查，错误挡在编译期
- **自举编译器** — 词法 / 语法 / 类型检查 / 代码生成全部用 TA 自己写，`make bootstrap-selfhost` 验证 fixed point（连续两次编译产物 byte-identical）
- **C 交互** — TA 是主语言，C 是胶水：C 模块通过 `import` 动态加载（`lib/http.c`、`lib/demo.c` 模板见 [docs/c-module.md](docs/c-module.md)）
- **内建模块** — `net` / `http` / `bufio` / `list` / `str` / `fmt` / `math`
- **轻量** — C VM 仅数千行（VM / 调度器 / 每进程 GC / NaN-boxing 值），完整实现见 [docs/design.md](docs/design.md)

## 快速上手

```console
$ make                    # 构建 tavm（C VM）+ C 模块
$ ./tinyactor run hello.ta
42
```

`tinyactor` 是命令行入口：

```
usage: tinyactor <command> [args...]

  build <src>.ta [<out>.tabc]   Compile .ta to .tabc (default: <src>.tabc)
  run   <src>.ta [args...]      Compile and run (build to /tmp, then tavm)
  fmt   <file>.ta               Format .ta in place (token-level, gofmt-style)
  fmt   --check <file>.ta       Check formatting; exits 1 if file differs
```

浏览器里零安装试玩：[Playground](docs/playground.html)（WASM 构建，见 `scripts/build-wasm.sh`）。

## 项目结构

| 路径 | 内容 |
|------|------|
| `src/` | C 实现的 VM：`vm.c`（字节码执行）、`scheduler.c`（调度器 / 进程 / 邮箱）、`gc.c`（每进程 semispace GC）、`val.c`（NaN-boxing）、`net.c` / `http.c`（网络内建模块） |
| `lib/` | TA 编译器源码（`tokenizer.ta` / `parser.ta` / `typecheck.ta` / `codegen.ta` / `driver.ta`）+ 标准库 + C 模块（`http.c` / `demo.c`）+ 编译产物 `bootstrap.tabc` |
| `test/` | 7 类测试脚本（basic / gc / actor / module / compiler / bootstrap / example） |
| `example/scripts/` | 可直接运行的示例（echo server、KV server、http server 等） |
| `docs/` | 设计文档、语言规范、Playground |
| `scripts/` | WASM 构建、教程验证（`verify-lessons.mjs`） |
| `benchmark/` | 性能基准 |

## 构建与测试

| 命令 | 说明 |
|------|------|
| `make` | 构建 `tavm` + C 模块 |
| `make test` | 7 类测试全量跑（basic / gc / actor / module / compiler / bootstrap / example） |
| `make bootstrap` | 用 `lib/bootstrap.tabc` 重新编译 TA 编译器，产物写回 `lib/bootstrap.tabc` |
| `make bootstrap-selfhost` | 自举 fixed point 验证：产物必须与 `bootstrap.tabc` byte-identical |
| `make test-gc-asan` / `make test-gc-tsan` | AddressSanitizer / ThreadSanitizer 下的 GC 测试（`ASAN=1` / `TSAN=1` 亦可单独构建） |
| `make fmt` / `make fmt-check` | 格式化 C/C++ 与 `lib/*.ta` / 校验格式 |
| `make benchmark` | 运行性能基准 |

## 示例

`example/scripts/` 里的脚本可直接跑：

```console
$ ./tinyactor run example/scripts/echo_server.ta    # TCP echo server
$ ./tinyactor run example/scripts/kv_server.ta      # 简单 KV server
```

`lib/serve.ta` 是一个静态文件 HTTP server（用内建 `net` 模块 + actor 并发处理连接）：

```console
$ tavm lib/serve.ta 8080 /path/to/www
```

## 文档

- [设计总览](docs/design.md)
- [TA 语言规范](docs/ta-language-spec.md)
- [分层类型模型](docs/layered-type-model.md)
- [泛型 ADT 设计](docs/generic-adt-design.md)
- [设计决策记录](docs/design-decisions.md)
- [C 模块编写指南](docs/c-module.md)
- [改进计划](docs/improvement-plan.md)
- [Typecheck 性能分析](docs/typecheck-performance.md)

## 路线图

supervisor → 热更新 → 持久化 → 分布式 → 标准库 → 性能（P5）→ DX（P6），完整见 [ROADMAP.md](ROADMAP.md)。

## FAQ（精选）

**和 Erlang / Go / Lua 有什么区别？**
Erlang 是 actor 模型，TinyActor 是极简化的 Erlang（VM 数千行 C），但语言层更强调类型安全；Go 是 CSP 模型，TinyActor 是 actor 模型；和 Lua 一样构建在 C 之上，但 Lua 适合嵌入 C，TinyActor 则是调用 C。

**带 GC 的语言是不是不行？**
GC 极大降低心智负担。TinyActor 走每进程独立 GC：无全局 STW，每个 actor 的停顿只影响自己，结合抢占式公平调度可做到软实时。

**为什么不直接用 Lisp / Python 语法？**
TA 的主要用户是 AI——AI 不喜欢大量括号配对，也不喜欢缩进即语义。

完整 FAQ 见主页（[中文](docs/index.md) / [English](docs/index-en.md)）。

## 项目状态

- 自举编译器 fixed point 已验证（`bootstrap.tabc ≡ bootstrap_selfhost.tabc`）
- 7 类测试全部通过，0 failures
- 路线图推进中，见 [ROADMAP.md](ROADMAP.md)