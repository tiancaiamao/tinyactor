# TinyActor 路线图

> 这里只放**还没做**的产品级事项，按优先级排。已完成的能力看
> [design.md](design.md)（现状与架构）与 [design-decisions.md](design-decisions.md)
> （决策与理由）；编译器/库的阶段性改进看 [improvement-plan.md](improvement-plan.md)；
> 标准库还缺什么以 `lib/*.ta` 的实际内容和 issue 为准。

## P0 — Supervisor / OTP-lite

现状：`lib/supervisor.ta` 已有一子进程监督——每个子进程一份 start thunk，
supervisor 监控每个子进程，DOWN 后只重启这一个（one_for_one），按每个子进程
自己的重启次数上限判强度（计数窗口，不是时间窗口）；超限时 supervisor 自身
退出，让它的父进程看到 DOWN。**现存语义以该文件的头部注释为准。**

还缺：

- 重启类型 permanent / transient / temporary（现在一律重启）
- one_for_all / rest_for_one 策略
- shutdown 策略（brutal_kill / timeout）

纯 TA 实现，不改 VM：supervisor 自己就是一个 actor，监听 DOWN + 重启子进程。

## P1 — 模块热更新

不停机替换模块代码。思路：VM 符号表分 current / old 两版；外部调用走 export 表
（更新后走新版），已在跑的内部调用继续旧版跑到自然结束，旧版无引用后 GC 回收。

前置：VM 符号表支持多版本 + compile.ta 区分内部/外部调用 + 闭包区分两种形态。

## P2 — 持久化

actor 状态序列化到磁盘、crash 后恢复。已有 `.tabc` 的字节码序列化格式，可扩展为
任意值序列化（int / symbol / string / pair；闭包另说）；增量 checkpoint 参考 BEAM
的进程状态 dump。前置：P0（由 supervisor 触发恢复）+ 序列化协议设计。

## P3 — 分布式

跨进程 / 跨机器 spawn 与消息透明传递。PID 要区分本地/远程，节点发现与连接管理
建在已有网络层上（`lib/net.ta` / `lib/tcp.ta`）。前置：P2（序列化）。

## P4 — 标准库扩充

原先这里列的是 `crypto`（哈希/随机数），已由 `lib/sha256.ta` + `lib/random.ta`
落地。之后的扩充按 issue 走，不再在这里维护一份会腐烂的手写清单。

## P5 — 性能优化

并行 / generational GC；可选 JIT（热点函数编译到 native）；更小的字节码
（指令压缩、常量池共享）；更快的模块加载（并行编译）。

## P6 — 开发者体验

- REPL —— 现在只有 `run` / `build` / `fmt` / `vendor` / `init` / `help`，没有
  交互式求值
- 调试器（断点 / 单步 / 变量查看）
- LSP（补全 / 诊断 / 跳转）——[improvement-plan.md](improvement-plan.md) 里明确
  「不着急」
- 错误信息继续优化（错误码与上下文已有，见 improvement-plan 阶段 B）

已落地：`tinyactor fmt` / `fmt --check`（token 级，gofmt 风格；执行体是
`lib/bootstrap/fmt.ta`）。