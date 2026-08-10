---
layout: default
title: An embeddable Actor-concurrency scripting language
lang: en
---
# TinyActor

**Actor-level concurrency for C programs — with a self-hosting compiler written in TA itself.**

TinyActor is a lightweight Actor language + VM implemented in C:
- **Concurrency model**: pure Actors (`spawn` / `send` / `recv`), process isolation — a crash never affects other processes;
- **Self-hosting compiler**: tokenizer → parser → typecheck → codegen are all written in TA, compiling to bytecode that runs on a C VM;
- **Built-in modules**: `net` (TCP), `http`, `bufio`, `list`, `str`, `fmt`, `math` and more.

Analogy: **Lua embeds in C → lightweight scripting, no concurrency; TinyActor embeds in C → lightweight scripting + Actor concurrency + a self-hosting compiler.**

## Core Features

- **Concurrent Actors**: `spawn` / `send` / `recv` / `self` / `monitor`; selective receive and `when` guards; crash isolation, with one-way `monitor` DOWN notifications as the foundation for supervisors
- **Multithreaded scheduler**: M:N scheduling (work-stealing), parallel across cores, reduction counting prevents starvation
- **GC**: per-process semispace copying GC, independent stop-the-world per process
- **Bootstrap fixed point**: `bootstrap.tabc ≡ bootstrap_selfhost.tabc` — the TA compiler compiles itself
- **Type system**: full Hindley-Milner inference + generic ADTs + exhaustive pattern matching
- **Module system**: `import` / `pub`, AOT incremental compilation cache (`.ta → .tabc`)
- **Runtime**: NaN-boxing value representation, stack-machine bytecode, tail-call optimization, closures
- **Embedding API**: `vm_new()` / `vm_load()` / `vm_run()` — embed into C programs like Lua

## Quick Start

Save the code below as `hello.ta` and run `./tinyactor run hello.ta` (prints `42`):

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

> This example is isomorphic to real tests under `test/actor/` (`self_send.ta`, `ping_pong.ta`).
> Keywords: `fn` / `let` / `match` / `if` / `spawn` / `send` / `recv`.

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

| Item | Status |
|------|--------|
| Bootstrap fixed point | ✅ Verified (`bootstrap.tabc ≡ bootstrap_selfhost.tabc`) |
| Compiler (tokenizer / parser / typecheck / codegen / driver) | ✅ Written in TA, ~7,300 LOC |
| Runtime & built-ins (vm / scheduler / gc / net / http …) | ✅ Written in C, ~5,200 LOC |
| Tests | ✅ 100+ test scripts across 7 suites: basic / actor / gc / module / compiler / bootstrap / example |
| Roadmap | P0 supervisor → P1 hot reload → P2 persistence → P3 distributed → P4 stdlib → P5 performance → P6 DX |

Total ≈ 13,000 LOC (ROADMAP milestone records ~11,000; current figures are from the code).
Full roadmap: [ROADMAP.md](https://github.com/tiancaiamao/tinyactor/blob/main/ROADMAP.md).