---
layout: default
title: A lightweight Actor language + VM
lang: en
---
<div class="hero">
  <p class="kicker">Yet another Erlang/Gleam? No BEAM — a lightweight VM of our own.</p>
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
    <h3>Actors are first-class</h3>
    <p>Concurrency is not a library or a framework — it's the core of the language. <code>spawn</code> / <code>send</code> / <code>recv</code> are syntax: no middle layer between you and concurrent code.</p>
  </div>
  <div class="card">
    <h3>No BEAM</h3>
    <p>Erlang's concurrency ideas are right, but BEAM is a decades-old monolith. We keep the essence and run it on our own lightweight VM: each actor has its own GC, so a pause only stalls that actor — never the whole system.</p>
  </div>
  <div class="card">
    <h3>TA is the main language, C is the glue</h3>
    <p>The opposite of Lua, where Lua serves C. Here C only writes base libraries and system interaction — TA is in charge, and talking to C is painless.</p>
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