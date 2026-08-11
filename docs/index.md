---
layout: default
title: 轻量级 Actor 语言 + VM
lang: zh
---
<div class="hero">
  <p class="kicker">又一门 Erlang/Gleam？理念相同，跑在自家轻量 VM 上</p>
  <h1>TinyActor 是一个轻量级 Actor 语言 + VM</h1>
  <p class="lead">函数式、类型安全，Erlang 风格的 Actor 并发跑在轻量字节码 VM 上，可嵌入 C、交互顺滑。</p>
  <div class="hero-actions">
    <a class="btn" href="#quick-start">快速上手</a>
    <a class="btn ghost" href="https://github.com/tiancaiamao/tinyactor">GitHub</a>
        <a class="btn ghost" href="{{ site.baseurl }}/playground.html">Playground</a>
  </div>
</div>

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
{:.hero-code}

## 为什么又是 TinyActor？

语言已经够多了。TinyActor 不靠堆特性，而靠一组**取舍**立足：

<div class="card-grid">
  <div class="card">
    <h3>Actor</h3>
    <p>Actor 模型是支持<strong>并发</strong>最正确的做法（之一）。并发是一等公民——<code>spawn</code> / <code>send</code> / <code>recv</code> 就是语法，写并发没有中间层。</p>
  </div>
  <div class="card">
    <h3>类型安全</h3>
    <p>这是一门函数式语言，类型安全保证写出来的代码更健壮：Hindley-Milner 类型推断 + 模式匹配穷尽性检查，把错误挡在编译期。</p>
  </div>
  <div class="card">
    <h3>C 交互</h3>
    <p>TA 是主语言，C 是胶水：C 只写基础库和系统交互，嵌入顺滑。与 Lua 相反——Lua 是 C 的附庸，TA 才是主角。</p>
  </div>
</div>

## 快速上手 {#quick-start}

把上面的示例存为 `hello.ta`，运行：

```console
./tinyactor run hello.ta
```

输出 `42`。`type` 定义消息的 ADT，`match` 解构消息，`spawn` / `send` / `recv` 做并发——类型安全与并发，一屏讲完。

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

## FAQ

<div class="faq">
  <details>
    <summary>字节码 VM 是不是很慢？</summary>
    <p>对，字节码 VM 确实比原生代码慢。但慢不慢是相对的：Python 是目前最流行的语言之一，解释器执行的性能并没有阻止它的流行。对绝大多数场景来说，字节码 VM 的性能已经足够。</p>
  </details>
  <details>
    <summary>带 GC 的语言是不是都不行？</summary>
    <p>不是。GC 极大地降低了写代码的心智负担，卡顿则有很多解法：Java 把 GC 优化到极致——增量 / 分代 / 并发收集，工程手段全用上了；Go 一直在强调低延迟 GC。但它们是<strong>全局 GC</strong>。TinyActor 走另一条路线：<strong>actor 独立 GC</strong>。没有全局 STW，每个 actor 的 GC 停顿只影响自己；每个 actor 持有的堆足够小，单次 GC 耗时足够低，停顿就不再是问题。结合抢占式公平调度，整体系统可以做到良好的软实时性。</p>
  </details>
  <details>
    <summary>为什么没有 REPL？</summary>
    <p>REPL 对人类写代码很有用——快速反馈提升效率。但在 AI 写代码的时代，REPL 并不会显著提升 AI 的效率。它在路线图上，只是优先级不高。</p>
  </details>
  <details>
    <summary>和 Erlang 比呢？</summary>
    <p>都是 actor 模型，TinyActor 是极简化的 Erlang——整个 VM 的 C 代码只有 4000 行左右的规模。语言层面 Erlang 是无类型的，TinyActor 更强调类型安全：Hindley-Milner 推断 + 模式匹配穷尽性检查。当然，OTP 和分布式生态是 Erlang 的护城河，TinyActor 不在这条赛道上。</p>
  </details>
  <details>
    <summary>和 Go 比呢？</summary>
    <p>并发模型不同：Go 实现的是 CSP，TinyActor 是 actor 模型，两者在并发支持上都表现优秀。定位也不同：Go 是全能型独立语言，TinyActor 是轻量上层语言，下层靠 C 提供基础设施。</p>
  </details>
  <details>
    <summary>和 Lua 比呢？</summary>
    <p>两者都构建在 C 之上，属于上层语言。Lua 更适合嵌入 C，TinyActor 则是调用 C。TinyActor 在 VM 层就处理好了线程，向上层提供 actor 抽象；Lua 把这层封装留给了使用者。</p>
  </details>
  <details>
    <summary>和 Scheme 比呢？</summary>
    <p>TinyActor 的外层语法经过 parser 解析后，内部 AST 以 Lisp 形态表示——内核和 Scheme 一样小：没有宏、没有 continuation。底层是无类型的，类型检查在上层语法中完成。</p>
  </details>
  <details>
    <summary>为什么不直接用 Lisp 语法？</summary>
    <p>因为 TA 的主要用户是 AI。AI 写代码不喜欢 <code>)))))))</code> 这种大量括号配对，在编辑工具处理上容易出错；也不喜欢 Python 的缩进「游标卡尺」，只是 AI 写多了 Python 才习惯；更不喜欢 YAML。</p>
  </details>
  <details>
    <summary>为什么跟 C 交互这么重要？</summary>
    <p>TinyActor 构建在字节码 VM 之上，字节码 VM 又构建在 C 和操作系统之上。需要一层向「上」屏蔽细节、向「下」沟通系统的边界。C 是这里最好的基础设施层——它是目前最恰到好处地描述硬件抽象的通用语言。只要现代操作系统还构建在 C 之上，这一层就是稳定的。</p>
  </details>
  <details>
    <summary>如果你还不信——</summary>
    <p>这里有一个 HTTP server 的实现（<code>lib/serve.ta</code>），实测跟 Go 对比，TA 的性能达到了 Go 的 1/4。这是一个解释器语言和编译型语言的比较。</p>
  </details>
  <details>
    <summary>你不是骗我的吧？</summary>
    <p>没骗你。这整个项目都是 AI 写的——不信你去问 AI 它有没有骗你。</p>
  </details>
</div>