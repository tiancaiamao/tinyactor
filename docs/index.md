---
layout: default
title: 嵌入式 Actor 并发脚本语言
lang: zh
---
# TinyActor

**给 C 程序加上 Actor 级别的并发能力，编译器自身用 TA 编写。**

TinyActor 是一个用 C 实现、可自举的轻量级 Actor 语言 + VM：
- **并发模型**：纯 Actor（`spawn` / `send` / `recv`），进程隔离，crash 不影响其他进程；
- **自举编译器**：tokenizer → parser → typecheck → codegen 全部用 TA 自身编写，编译到字节码在 C VM 上运行；
- **内建模块**：`net`（TCP）、`http`、`bufio`、`list`、`str`、`fmt`、`math` 等。

类比：**Lua 嵌入 C → 轻量脚本，无并发；TinyActor 嵌入 C → 轻量脚本 + Actor 并发 + 自举编译器。**

## 核心特性

- **并发 Actor**：`spawn` / `send` / `recv` / `self` / `monitor`；选择性接收（selective receive）、`when` 守卫；进程崩溃隔离，`monitor` 单向 DOWN 通知是 supervisor 的基础
- **多线程调度**：M:N 调度器（work-stealing），多核并行，reduction 计数防饥饿
- **GC**：per-process semispace 拷贝 GC，进程独立 stop-the-world
- **自举 fixed point**：`bootstrap.tabc ≡ bootstrap_selfhost.tabc`，TA 编译器可自编译
- **类型系统**：完整 Hindley-Milner 推断 + 泛型 ADT + 模式匹配穷尽性检查
- **模块系统**：`import` / `pub`，`.ta → .tabc` AOT 增量编译缓存
- **运行时**：NaN-boxing 值表示、栈机字节码、尾调用优化、闭包捕获
- **嵌入 API**：`vm_new()` / `vm_load()` / `vm_run()`，像 Lua 一样嵌入 C 程序

## 快速上手

把下面的代码存为 `hello.ta`，然后运行 `./tinyactor run hello.ta`（输出 `42`）：

```ta
// 最小 Actor 示例：spawn 一个 worker，用 send/recv 收发消息
fn worker() {
  let msg = recv()          // 收消息
  send(self(), msg + 1)     // 回一条消息给自己
  print(recv())             // 再收并打印
}

fn main() {
  let pid = spawn(fn { worker() })  // 启动一个 actor
  send(pid, 41)                     // 给它发消息
  let ref = monitor(pid)            // 等 worker 结束
  recv()
}
```

> 本示例与 `test/actor/` 下的真实测试同构（`self_send.ta`、`ping_pong.ta`），
> 语法关键字：`fn` / `let` / `match` / `if` / `spawn` / `send` / `recv`。

## 文档

- [设计总览]({{ site.baseurl }}/design.html)
- [TA 语言规范]({{ site.baseurl }}/ta-language-spec.html)
- [分层类型模型]({{ site.baseurl }}/layered-type-model.html)
- [泛型 ADT 设计]({{ site.baseurl }}/generic-adt-design.html)
- [设计决策记录]({{ site.baseurl }}/design-decisions.html)
- [改进计划]({{ site.baseurl }}/improvement-plan.html)
- [C 模块编写指南]({{ site.baseurl }}/c-module.html)
- [Typecheck 性能分析]({{ site.baseurl }}/typecheck-performance.html)

## 项目状态

| 项目 | 状态 |
|------|------|
| 自举 fixed point | ✅ 已验证（`bootstrap.tabc ≡ bootstrap_selfhost.tabc`） |
| 编译器（tokenizer / parser / typecheck / codegen / driver） | ✅ TA 编写，约 7,300 行 |
| 运行时与内建模块（vm / scheduler / gc / net / http …） | ✅ C 编写，约 5,200 行 |
| 测试 | ✅ 100+ 个测试脚本，basic / actor / gc / module / compiler / bootstrap / example 七类全通过 |
| 后续路线 | P0 supervisor → P1 热更新 → P2 持久化 → P3 分布式 → P4 标准库 → P5 性能 → P6 DX |

代码合计约 1.3 万行（ROADMAP 里程碑记录为 ~11000，当前数字以代码为准）。
完整路线图见 [ROADMAP.md](https://github.com/tiancaiamao/tinyactor/blob/main/ROADMAP.md)。