---
layout: default
title: 轻量级 Actor 语言 + VM
lang: zh
---
<div class="hero">
  <p class="kicker">又一门 Erlang/Gleam？不要 BEAM——跑在自家轻量 VM 上</p>
  <h1>TinyActor 是一个轻量级 Actor 语言 + VM</h1>
  <p class="lead">函数式、类型安全，Erlang 风格的 Actor 并发跑在轻量字节码 VM 上，可嵌入 C、交互顺滑。</p>
  <div class="hero-actions">
    <a class="btn" href="#quick-start">快速上手</a>
    <a class="btn ghost" href="https://github.com/tiancaiamao/tinyactor">GitHub</a>
    <span class="btn ghost disabled" aria-disabled="true">Playground · 即将上线</span>
  </div>
</div>

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
{:.hero-code}

## 为什么又是 TinyActor？

语言已经够多了。TinyActor 不靠堆特性，而靠一组**取舍**立足：

<div class="card-grid">
  <div class="card">
    <h3>Actor 是一等公民</h3>
    <p>并发不是库、不是框架，而是语言核心。<code>spawn</code> / <code>send</code> / <code>recv</code> 就是语法——写并发没有中间层。</p>
  </div>
  <div class="card">
    <h3>不要 BEAM</h3>
    <p>Erlang 的并发理念是对的，但 BEAM 是几十年的庞然大物。我们只取精华，跑在自己的轻量 VM 上：每个 actor 独立 GC，卡顿只卡自己，不卡全局。</p>
  </div>
  <div class="card">
    <h3>TA 是主语言，C 是胶水</h3>
    <p>与 Lua 相反：Lua 是 C 的附庸，TA 才是主角。C 只写基础库和系统交互，嵌入 C 顺滑，交互方便。</p>
  </div>
</div>

## 快速上手 {#quick-start}

把上面的示例存为 `hello.ta`，运行：

```console
./tinyactor run hello.ta
```

输出 `42`。`spawn` 启动一个 actor，`send` 发消息，`recv` 收消息——并发就是语法本身。

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

- **自举**：编译器（词法 / 语法 / 类型检查 / 代码生成）全部用 TA 自己写，编译为字节码在自己的 VM 上运行
- **类型安全**：Hindley-Milner 类型推断 + 泛型 ADT + 模式匹配穷尽性检查
- **模块与内建**：`import` / `pub`，内建 `net` / `http` / `bufio` / `list` / `str` / `fmt` / `math`
- **测试**：basic / actor / gc / module / compiler / bootstrap / example 七类测试全部通过
- **路线图**：supervisor → 热更新 → 持久化 → 分布式 → 标准库 → 性能 → DX

完整路线图见 [ROADMAP.md](https://github.com/tiancaiamao/tinyactor/blob/main/ROADMAP.md)。