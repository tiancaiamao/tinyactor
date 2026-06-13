# Spec: TinyActor Phase 3 — 模块化 + 系统级集成

## Status: 🔄 IN PROGRESS

## Goal
将 TinyActor 从玩具项目升级为实用工具：引入模块系统让脚本和 C 协作构建真实应用，通过 TCP echo server + HTTP server 示例证明 actor 并发模型处理网络 I/O 的能力。

## Design Decisions

### 模块系统
- **C 模块**：`vm_register_module(vm, "net", funcs[])` 注册一组 C 函数为命名模块
- **脚本使用**：`(import "net")` 导入模块，函数以 `module.func` 形式调用（如 `(net.listen 8080)`）
- **脚本模块**（可选）：`(require "utils")` 加载 `utils.lisp`，导出的函数可使用
- 编译器识别 `import` 特殊形式，将 `module.func` 调用编译为 OP_CCALL（复用现有机制）

### I/O 调度模型（单线程事件驱动）
- 所有 socket 设为 non-blocking
- C 函数（net.accept, net.read 等）尝试操作，如果 would-block 则返回特殊值 `'would-block`
- 脚本 actor 收到 `'would-block` 后可 yield（进入 PROC_WAIT_IO 状态）
- 调度器在所有 actor 都等待时，用 poll() 等待 I/O 就绪，唤醒对应 actor
- Actor 代码看起来是同步的：`(let conn (net.accept server))` — 阻塞当前 actor 直到有连接

### 与 Erlang 的对照
- 不做 try/catch（Erlang 哲学是 "Let it crash"，TinyActor 已有 monitor + supervisor）
- 模块系统类似 Erlang 的 `-module` + `-export`
- I/O 模型类似 Erlang 的 port driver

## Acceptance Criteria

### L1 — Structural（结构完整性）

- [ ] `make clean && make` 零 error 编译 — Verify: `make clean && make 2>&1 | grep -c error`
- [ ] Phase 2 的 42 个通过的测试不退化 — Verify: `cd test && ./run_all.sh` 42 pass
- [ ] `ta.h` 包含 `vm_register_module` 声明和 `TaModule` 结构体 — Verify: `grep vm_register_module ta.h`
- [ ] `ta.h` 包含 `TaIOWait` 或等效的 I/O 等待结构 — Verify: `grep -E 'IOWait|io_wait|PROC_WAIT_IO' ta.h`
- [ ] `src/module.c` 新文件存在 — Verify: `test -f src/module.c`
- [ ] `src/net.c` 新文件存在 — Verify: `test -f src/net.c`
- [ ] `Makefile` 包含 `src/module.o` 和 `src/net.o` — Verify: `grep 'module.o\|net.o' Makefile`
- [ ] `example/echo_server.c` 存在且可编译 — Verify: `cd example && make echo_server`
- [ ] `example/http_server.c` 存在且可编译 — Verify: `cd example && make http_server`
- [ ] preempt.lisp exit 0 — Verify: `timeout 5 ./tinyactor test/scripts/preempt.lisp; echo $?`

### L2 — Behavioral（行为正确性）

#### L2.1 — Preempt Bug Fix（Phase 2 遗留）
- [ ] preempt.lisp 输出 "ok" 且 exit 0 — Verify: `timeout 5 ./tinyactor test/scripts/preempt.lisp`
- [ ] 根进程退出后，无限循环的子进程被正确清理
- [ ] 不影响其他正常进程的调度

#### L2.2 — 模块系统
- [ ] C 端：`vm_register_module(vm, "test", funcs)` 成功注册 — Verify: module_test.lisp 通过
- [ ] 脚本端：`(import "test")` 后可调用 `(test.hello)` — Verify: module_test.lisp 输出 "hello from C"
- [ ] 模块函数接收参数并返回值：`(test.add 3 4)` → 7 — Verify: module_test.lisp 输出 "7"
- [ ] 模块函数可触发 GC（返回 string/pair 等堆分配值）— Verify: module_test.lisp 输出正确
- [ ] 编译错误：`(import "nonexist")` 报错而非崩溃 — Verify: 无 segfault

#### L2.3 — 网络 C 模块
- [ ] `(net.listen port)` 返回 server socket — Verify: echo_server 示例可启动
- [ ] `(net.accept server)` 非阻塞接受连接，无连接时 actor 挂起 — Verify: echo_server 示例可接受连接
- [ ] `(net.read fd)` 非阻塞读取数据 — Verify: echo_server 示例可读取客户端数据
- [ ] `(net.write fd data)` 非阻塞写入数据 — Verify: echo_server 示例可回复客户端
- [ ] `(net.close fd)` 关闭 socket — Verify: 无资源泄漏

#### L2.4 — I/O 调度器
- [ ] 单个 actor 在 I/O 等待时，其他 actor 正常运行 — Verify: 多连接并发测试
- [ ] 调度器在所有 actor 等待 I/O 时 poll()，有就绪 fd 时唤醒对应 actor
- [ ] I/O 完成后 actor 正确恢复执行，返回值正确

#### L2.5 — TCP Echo Server 示例
- [ ] 启动 echo server，监听指定端口
- [ ] 多个客户端同时连接，每个连接由独立 actor 处理
- [ ] 客户端发送数据，服务器原样回复
- [ ] 测试脚本：启动 server + N 个 client actor，每个 client 发送-接收-验证
- [ ] 客户端和服务器通过真实 TCP socket 交互（非进程内通信）

#### L2.6 — HTTP Server 示例
- [ ] 启动 HTTP server，监听指定端口
- [ ] 接受 HTTP GET 请求，返回简单 HTML 响应（如 "Hello from TinyActor"）
- [ ] 可同时处理多个并发请求（actor-per-connection）
- [ ] 用 curl 或 telnet 可验证 — Verify: `curl http://localhost:PORT/`
- [ ] 演示路由：不同路径返回不同内容（至少 2 条路由）

#### L2.7 — try/catch（可选，如时间允许）
- [ ] `(try expr (catch e handler))` 捕获运行时错误 — Verify: try_catch_test.lisp
- [ ] catch 后进程正常继续运行
- [ ] 未捕获的错误仍然导致进程死亡 + monitor 通知

## Constraints
- 单线程调度，不做多线程
- 网络模块仅支持 TCP（不做 UDP）
- HTTP server 是最小实现（解析 request line + headers，发送 response），不做完整 HTTP 协议
- 代码风格与 Phase 1/2 一致（C99，Wall-Wextra clean）
- 所有新功能必须通过测试

## Out of Scope
- 多线程调度（Phase 4）
- UDP 支持
- 完整 HTTP/1.1 协议实现
- TLS/SSL
- bytes 类型（Phase 2 遗留，可后续做）
- 嵌套 pattern matching / guard（独立特性，不阻塞本阶段）

## Task Breakdown

### Task 1: Preempt Bug Fix（Phase 2 遗留，最高优先级）
- 修改 vm_run：根进程退出后清理所有子进程（或：所有非 WAIT_RECV 的死循环进程）
- 确保 preempt.lisp exit 0
- 不影响正常的多进程调度
- 估计：~30 行改动

### Task 2: 模块系统
- 新文件 `src/module.c`：TaModule 结构体、vm_register_module、模块查找
- 编译器：`(import "name")` 特殊形式、`module.func` 符号解析
- 复用 OP_CCALL 调度，模块函数注册到 vm->cfuncs 数组
- 测试：module_test.lisp
- 估计：~150 行新代码

### Task 3: 网络 C 模块 + I/O 调度器
- 新文件 `src/net.c`：net.listen/accept/read/write/close 实现
- ta.h：PROC_WAIT_IO 状态、io_wait 队列
- vm.c：调度器 poll() 集成，actor I/O 阻塞/唤醒机制
- 所有 socket non-blocking
- 估计：~250 行新代码

### Task 4: TCP Echo Server 示例
- `example/echo_server.c`：C main 注册 net 模块 + 加载脚本
- `example/scripts/echo_server.lisp`：server actor + client actor
- `example/Makefile`：独立编译
- 测试脚本：自动启动 server + client，验证 echo
- 估计：~120 行新代码

### Task 5: HTTP Server 示例
- `example/http_server.c`：C main 注册 net 模块 + http 辅助函数
- `example/scripts/http_server.lisp`：路由 + handler actors
- 演示路由 + 并发处理
- 估计：~150 行新代码

### Task 6: 测试 + 独立验收
- 补充测试脚本（preempt fix、module、net operations、try/catch）
- Phase 2 回归测试（42 个仍通过）
- Evaluator 独立验收
- Review agent 代码审查

## Estimated Total
- 新增代码：~700 行
- 修改代码：~50 行
- 新增文件：src/module.c, src/net.c, example/echo_server.c, example/http_server.c, example/scripts/*.lisp, test/scripts/*.lisp