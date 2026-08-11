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
        <a class="btn ghost" href="{{ site.baseurl }}/playground.html">Playground</a>
  </div>
</div>

```ta
// An ADT shapes the message; match destructures it by pattern
type Msg { Add(a, b); Mul(a, b) }

fn worker() {
  match recv() {
    Add(a, b) -> print(a + b)   // Add(20, 22) arrives → 42
    Mul(a, b) -> print(a * b)
  }
}

fn main() {
  let pid = spawn(fn { worker() })  // concurrency: spawn an actor
  send(pid, Add(20, 22))            // messaging: the message is an ADT value
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
    <h3>C interop</h3>
    <p>TA is the main language, C is the glue: C only implements base libraries and OS interop, and embedding is seamless. The opposite of Lua, where Lua serves C.</p>
  </div>
</div>

## Quick Start {#quick-start}

Save the example above as `hello.ta` and run:

```console
./tinyactor run hello.ta
```

It prints `42`. `type` defines the message as an ADT, `match` destructures it, and `spawn` / `send` / `recv` handle concurrency — type safety and concurrency in one screen.

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

## FAQ

<div class="faq">
  <details>
    <summary>Isn't a bytecode VM slow?</summary>
    <p>Yes, a bytecode VM is slower than native code. But "slow" is relative: Python is one of the most popular languages today, and interpreter performance hasn't stopped its rise. For the vast majority of use cases, bytecode VM performance is more than enough.</p>
  </details>
  <details>
    <summary>Aren't languages with GC bad?</summary>
    <p>No. GC dramatically lowers the mental burden of writing code, and pauses have many solutions: Java pushes GC to the limit — incremental / generational / concurrent collection, every engineering trick in the book; Go has long emphasized low-latency GC. But those are <strong>global GCs</strong>. TinyActor takes a different route: <strong>per-actor GC</strong>. There is no global stop-the-world — an actor's GC pause only affects that actor. When each actor's heap is small enough, a single GC is fast enough, and pauses stop being a problem. Combined with preemptive fair scheduling, the whole system can achieve good soft real-time behavior.</p>
  </details>
  <details>
    <summary>Why no REPL?</summary>
    <p>A REPL is great for humans writing code — fast feedback boosts productivity. But in the age of AI writing code, a REPL doesn't meaningfully speed up AI. It's on the roadmap, just not a priority.</p>
  </details>
  <details>
    <summary>How does it compare to Erlang?</summary>
    <p>Same actor model — TinyActor is a minimal Erlang: the whole VM is on the order of ~4000 lines of C. At the language level Erlang is untyped; TinyActor leans harder into type safety: Hindley-Milner inference + exhaustive pattern matching. Of course, OTP and the distributed ecosystem are Erlang's moat — TinyActor isn't competing on that turf.</p>
  </details>
  <details>
    <summary>How about Go?</summary>
    <p>Different concurrency models: Go implements CSP, TinyActor is actor-based, and both are excellent at concurrency. Different positioning too: Go is a versatile standalone language; TinyActor is a lightweight high-level language that leans on C for infrastructure.</p>
  </details>
  <details>
    <summary>How about Lua?</summary>
    <p>Both sit on top of C as high-level languages. Lua is designed to be embedded in C; TinyActor calls into C. TinyActor handles threads at the VM layer and exposes an actor abstraction upward; Lua leaves that layer to the user.</p>
  </details>
  <details>
    <summary>How about Scheme?</summary>
    <p>TinyActor's surface syntax is parsed into a Lisp-form internal AST — a kernel as small as Scheme: no macros, no continuations. The low level is untyped; type checking happens in the surface syntax.</p>
  </details>
  <details>
    <summary>Why not just use Lisp syntax?</summary>
    <p>Because TA's primary user is AI. AI doesn't like writing <code>)))))))</code> — all those matching parens are error-prone in editor tooling. It doesn't like Python's indentation ruler either — it's just used to it after writing so much Python. And it likes YAML even less.</p>
  </details>
  <details>
    <summary>Why does C interop matter so much?</summary>
    <p>TinyActor sits on a bytecode VM, which sits on C and the OS. You need a boundary that hides details going up and communicates going down. C is the best infrastructure layer here — the universal language that most precisely describes hardware abstraction. As long as modern operating systems are built on C, this layer stays stable.</p>
  </details>
  <details>
    <summary>Still not convinced?</summary>
    <p>There's an HTTP server implementation (<code>lib/serve.ta</code>). In a real benchmark against Go, TA reached 1/4 of Go's performance — an interpreter compared to a compiled language.</p>
  </details>
  <details>
    <summary>Are you kidding me?</summary>
    <p>Nope. The entire project was written by AI — if you don't believe me, go ask the AI whether it lied to you.</p>
  </details>
</div>