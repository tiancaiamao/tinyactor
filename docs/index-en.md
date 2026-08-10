---
layout: default
title: A lightweight Actor language + VM
lang: en
---
<div class="hero">
  <p class="kicker">Yet another Erlang/Gleam? Same ideas — on a lightweight VM of our own.</p>
  <h1>TinyActor is a lightweight Actor language + VM</h1>
  <p class="lead">Functional, type-safe, Erlang-style actor concurrency on a lightweight bytecode VM — embeddable in C with easy interop.</p>
  <div class="hero-actions">
    <a class="btn" href="#quick-start">Quick start</a>
    <a class="btn ghost" href="https://github.com/tiancaiamao/tinyactor">GitHub</a>
    <span class="btn ghost disabled" aria-disabled="true">Playground · coming soon</span>
  </div>
</div>

```ta
// Minimal Actor example: spawn a worker, exchange messages with send/recv
fn worker() {
  let msg = recv()          // receive a message
  send(self(), msg + 1)     // send one back to ourselves
  print(recv())             // receive it again and print
}

fn main() {
  let pid = spawn(fn { worker() })  // spawn an actor
  send(pid, 41)                     // send it a message
  let ref = monitor(pid)            // wait for the worker to finish
  recv()
}
```
{:.hero-code}

## Why another language?

There are already plenty of languages. TinyActor doesn't stack features — it stands on a few **trade-offs**:

<div class="card-grid">
  <div class="card">
    <h3>Actors</h3>
    <p>The actor model is one of the most correct ways to do <strong>concurrency</strong>. Concurrency is a first-class citizen — <code>spawn</code> / <code>send</code> / <code>recv</code> are syntax, with no middle layer between you and concurrent code.</p>
  </div>
  <div class="card">
    <h3>Type safety</h3>
    <p>A functional language where type safety makes code more robust: Hindley-Milner inference and exhaustive pattern matching catch errors at compile time.</p>
  </div>
  <div class="card">
    <h3>GC without the mental burden</h3>
    <p>GC removes the burden of manual memory management. <strong>Per-actor GC</strong> — no global stop-the-world; a GC pause only affects that one actor.</p>
  </div>
  <div class="card">
    <h3>C interop</h3>
    <p>TA is the main language, C is the glue: C only implements base libraries and OS interop, and embedding is seamless. The opposite of Lua, where Lua serves C.</p>
  </div>
</div>

## Quick Start {#quick-start}

Save the example above as `hello.ta` and run:

```console
./tinyactor run hello.ta
```

It prints `42`. `spawn` starts an actor, `send` sends it a message, `recv` receives one — concurrency is just syntax.

## Documentation

- [Design Overview]({{ site.baseurl }}/design.html)
- [TA Language Specification]({{ site.baseurl }}/ta-language-spec.html)
- [Layered Type Model]({{ site.baseurl }}/layered-type-model.html)
- [Generic ADT Design]({{ site.baseurl }}/generic-adt-design.html)
- [Design Decisions]({{ site.baseurl }}/design-decisions.html)
- [Improvement Plan]({{ site.baseurl }}/improvement-plan.html)
- [C Module Authoring Guide]({{ site.baseurl }}/c-module.html)
- [Typecheck Performance Analysis]({{ site.baseurl }}/typecheck-performance.html)

## Project Status

- **Self-hosting**: the compiler (lexer / parser / typecheck / codegen) is written in TA itself, compiling to bytecode that runs on its own VM
- **Type safety**: Hindley-Milner inference + generic ADTs + exhaustive pattern matching
- **Modules & built-ins**: `import` / `pub`, with built-in `net` / `http` / `bufio` / `list` / `str` / `fmt` / `math`
- **Tests**: 7 suites all passing — basic / actor / gc / module / compiler / bootstrap / example
- **Roadmap**: supervisor → hot reload → persistence → distributed → stdlib → performance → DX

Full roadmap: [ROADMAP.md](https://github.com/tiancaiamao/tinyactor/blob/main/ROADMAP.md).