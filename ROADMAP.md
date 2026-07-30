# TinyActor ROADMAP

## 现状

| 模块 | 语言 | 行数 | 状态 |
|---

## 代码清理（Code Cleanup）

| 任务 | 状态 |
|------|------|
| A1. 拆分 vm.c → scheduler.c + vm.c | ✅ |
| A2. ta.h 分层 — static inline 移入 ta_inline.h | ✅ |
| B1. gc_root 守卫宏 — GC_ROOTS_SCOPE | ✅ |
| B2. 统一错误处理 — 返回约定规范化 | ⬜ |
| B3. buf.c 生命周期 — 支持槽位复用 | ⬜ |
| C1. 清理 test stubs — test_hello/test_add | ⬜ |
| C2. tinyactor 缩进/mktemp 修复 | ✅ |
| C3. tok_vecs 溢出警告 | ⬜ |
| C4. 注释/格式清理 | ⬜ |

---|------|------|------|
| tokenizer.ta | TA | 348 | ✅ |
| parser.ta | TA | 1097 | ✅ |
| codegen.ta | TA | 1628 | ✅ |
| typecheck.ta (HM 类型推断) | TA | 2027 | ✅ |
| driver.ta (模块解析 + 管线编排) | TA | 198 | ✅ |
| vm.c (opcode dispatch) | C | 841 | ✅ |
| scheduler.c (调度器/进程/邮箱) | C | 574 | ✅ |
| gc.c (per-process semispace GC) | C | 248 | ✅ |
| val.c (NaN-boxing) | C | 225 | ✅ |
| api.c / buf.c / file.c / str.c / net.c / http.c | C | ~1500 | ✅ |
| **合计** | | **~11000** | |

**自举固定点已验证**：`bootstrap.tabc ≡ bootstrap_selfhost.tabc`

**201 个测试全通过**（含类型检查、ADT、模式匹配、模块加载、GC 压力、多线程、网络）

---

## 已完成的核心里程碑

### ✅ 自举 (Bootstrap)
- 编译器全部用 TA 自身编写：tokenizer → parser → codegen → typecheck
- C 侧只保留 VM 核心 + 内置模块
- `compile.c` / `reader_ta.c` 已移除
- 固定点验证通过，TA 编译器可自编译

### ✅ 语法 (Phase: New Syntax)
- `.ta` 是用户语言（ML/Rust 系语法），`.lisp` 支持已移除
- 关键字：`fn` / `let` / `match` / `if` / `spawn` / `send` / `recv`
- `snake_case` 函数命名，大括号分组
- 模式匹配 desugar 到 if + and + 谓词（parser.ta 中完成）

### ✅ 类型系统 (ADT + Hindley-Milner)
- 代数数据类型（`type Color { Red; Green; Blue }`）
- 类型标注（`fn add(a: Int, b: Int) -> Int`）
- 完整 Hindley-Milner 类型推断（含泛型、let-polymorphism）
- 模式匹配穷尽性检查
- 类型错误报告（含位置信息）
- 类型信息纯编译期，不进入运行时字节码

### ✅ 模块系统
- `import` 语句递归加载 `.ta` 文件
- `pub` 可见性控制
- 模块搜索路径：当前目录 → `lib/`
- C 内置模块（net / http / file / str / buf / vm）保留为 FFI 原语
- AOT 缓存：`.ta → .tabc` 增量编译

### ✅ VM 核心 (VM Core)
- NaN-boxing 值表示
- 寄存器机字节码解释器
- per-process semispace GC（stop-the-world）
- 多线程调度器（work-stealing）
- Actor 原语：spawn / send / recv / monitor / self
- Selective receive（RECV_PEEK / RECV_COMMIT）
- Tail call 优化
- 闭包 + 自由变量捕获
- 进程隔离（crash 不影响其他进程）

### ✅ 内置模块
- **net**: TCP listen / accept / read / write（非阻塞 I/O + poller）
- **http**: HTTP request 解析 + response 构造
- **file**: 文件读写
- **str**: 字符串操作（concat / substr / char_at / eq / to_sym）
- **buf**: 字节缓冲区
- **vm**: parse_source / load_bytecode / get_arg / is_builtin_module / tok_type / tok_val

---

## 待完成

### P0 — Supervisor / OTP-lite

Erlang 风格监督树。当前 actor crash 后只发 DOWN 消息，无自动重启。

```
// one_for_one: 子进程 crash 后重启
supervisor.start_link(fn {
  children: [
    { id: 'http_server, start: fn { accept_loop(8080) }, restart: :permanent },
    { id: 'health_check, start: fn { health_check_loop() }, restart: :transient },
  ],
  strategy: :one_for_one
})
```

#### 实现思路
- 纯 TA 实现，不改 VM
- Supervisor 本身是一个 actor，监听 DOWN 消息 + 重启子进程
- 支持 restart 策略：permanent / transient / temporary
- 支持 shutdown 策略：brutal_kill / timeout(N)
- 可以参考现有 `test/scripts/error-supervisor-restart.ta` 中的手动模式自动化

#### 前置依赖
- 无（纯 library 实现）

---

### P1 — 模块热更新 (Hot Reload)

不停机替换模块代码。

```
reload_module("http")   // 不停机替换 http 模块的代码
```

#### 实现思路
- VM 支持多版本符号表（current + old）
- 外部调用走 export 表，更新后走新版本
- 内部调用继续旧版本代码，跑完自然结束
- 旧版本无引用后 GC 回收

#### 前置依赖
- VM 改造：符号表支持多版本
- codegen 改造：区分 internal call / external call
- 闭包区分内部/外部形态

---

### P2 — 持久化 (Persistence)

Actor 状态序列化到磁盘，crash 后恢复。

```
let state = save_state(my_actor)
file.write("checkpoint.bin", state)

let my_actor = restore_state(file.read("checkpoint.bin"))
```

#### 实现思路
- 已有 bytecode 序列化格式（.tabc）
- 扩展为任意值序列化（pair / string / int / closure）
- 增量 checkpoint（参考 BEAM 的进程状态 dump）

#### 前置依赖
- P0（supervisor 触发恢复）
- 序列化协议设计

---

### P3 — 分布式 (Distributed)

跨进程/跨机器创建 actor，消息透明传递。

```
let pid = spawn("node2@example.com", fn { worker() })
send(pid, Msg("hello"))
```

#### 实现思路
- PID 编码区分本地/远程
- 网络层基于 net.c（已有 TCP 支持）
- 序列化协议复用 P2 的方案
- 节点发现 + 连接管理

#### 前置依赖
- P2（序列化）
- 网络层（已有 net.c）

---

### P4 — 标准库扩充

当前标准库偏少，以下模块待补充：

| 模块 | 说明 |
|------|------|
| `list.ta` | map / filter / foldl / foldr / zip / flatten |
| `result.ta` | Result monad（Ok / Err 的链式操作） |
| `option.ta` | Option monad（Some / None） |
| `json.ta` | JSON 解析 + 序列化 |
| `crypto.ta` | 哈希 / 随机数 |

---

### P5 — 性能优化

| 项目 | 说明 |
|------|------|
| 更快 GC | 并行 GC / generational GC |
| JIT | 热点函数编译到 native（可选） |
| 更小的字节码 | 指令压缩 / 常量池共享 |
| 更快的模块加载 | 延迟编译 / 并行编译 |

---

### P6 — 开发者体验

| 项目 | 说明 |
|------|------|
| REPL | 交互式命令行 |
| 调试器 | 断点 / 单步 / 变量查看 |
| 格式化器 | 自动格式化 `.ta` 代码 |
| LSP 协议 | 编辑器支持（补全 / 诊断 / 跳转） |
| 错误信息优化 | 更友好的类型错误 + 编译错误 |

---

## 架构概览

```
┌─────────────────────────────────────────────────────┐
│                   用户代码 (.ta)                      │
├─────────────────────────────────────────────────────┤
│  tokenizer.ta → parser.ta → typecheck.ta → codegen.ta│  ← TA 编译器（自举）
├─────────────────────────────────────────────────────┤
│                  bytecode (.tabc)                    │
├─────────────────────────────────────────────────────┤
│  vm.c (解释器 + 调度器 + GC)  ←  C 运行时（~2800 行）  │
│  api.c / buf.c / file.c / str.c / net.c / http.c    │
└─────────────────────────────────────────────────────┘

C 的职责：VM 核心 + 内置模块 FFI
TA 的职责：编译器 + 类型检查 + 逻辑编排
bootstrap.tabc：TA 编译器的预编译字节码（种子）
```

## 仓库结构

```
src/
  vm.c         字节码解释器 + actor 调度器
  gc.c         per-process semispace GC
  val.c        NaN-boxing 值表示 + 类型谓词
  api.c        VM 自省模块（load_bytecode / parse_source 等）
  buf.c        字节缓冲区
  str.c        字符串操作
  file.c       文件 I/O
  net.c        TCP 网络
  http.c       HTTP 解析
  main.c       CLI 入口
lib/
  tokenizer.ta   词法分析器
  parser.ta      语法分析器（含 pattern desugar）
  codegen.ta     字节码生成器
  typecheck.ta   Hindley-Milner 类型检查器
  driver.ta      模块解析 + 编译管线编排
  bootstrap.tabc 种子编译器（fixed point verified）
  bootstrap_selfhost.tabc  自举验证产物
test/
  scripts/      68 个测试脚本
  run_all_tests.sh  测试运行器
```

## 关键设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 自举策略 | 多层自举（C → .tabc → .ta） | 消除重复实现 |
| 类型系统 | Hindley-Milner（非 gradual） | 类型安全 + 推导完备 |
| 模块加载 | 递归 resolve + rebase fn_id | 运行时零开销 |
| 并发模型 | Actor（非共享内存） | 进程隔离 + 分布式友好 |
| GC | per-process semispace（非全局 tracing） | 低延迟 + 进程独立 |