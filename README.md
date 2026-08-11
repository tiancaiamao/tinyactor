# TinyActor

A lightweight Actor language + VM. Functional and type-safe, with Erlang-style actor concurrency running on a self-hosted compiler and a lightweight bytecode VM. Embeddable in C.

## The language in one screen

```ta
// An ADT defines the two shapes a message can take; match destructures them
type Msg { Add(a, b); Mul(a, b) }

fn worker() {
  match recv() {
    Add(a, b) -> print(a + b)   // Add(20, 22) received → 42
    Mul(a, b) -> print(a * b)
  }
}

fn main() {
  let pid = spawn(fn { worker() })  // concurrency: start an actor
  send(pid, Add(20, 22))            // communication: messages are ADT values
}
```

```console
$ ./tinyactor run hello.ta
42
```

## Features

- **Actor concurrency as a first-class citizen** — `spawn` / `send` / `recv` are syntax, not a library. Each actor has its own GC: no global stop-the-world.
- **Type safety** — Hindley-Milner type inference + generic ADTs + exhaustive pattern matching. Errors are caught at compile time.
- **Self-hosted compiler** — the lexer / parser / typechecker / codegen are all written in TA itself. `make bootstrap-selfhost` verifies the fixed point: two consecutive builds produce byte-identical artifacts.
- **C interop** — TA is the protagonist, C is the glue: C modules are dynamically loaded via `import` (`lib/http.c`, `lib/demo.c` template — see [docs/c-module.md](docs/c-module.md)).
- **Builtin modules** — `net` / `http` / `bufio` / `list` / `str` / `fmt` / `math`.
- **Lightweight** — the C VM is only a few thousand lines (VM / scheduler / per-process GC / NaN-boxed values). Full design in [docs/design.md](docs/design.md).

## Quick start

```console
$ make                    # build tavm (the C VM) + C modules
$ ./tinyactor run hello.ta
42
```

`tinyactor` is the command-line entry point:

```
usage: tinyactor <command> [args...]

  build <src>.ta [<out>.tabc]   Compile .ta to .tabc (default: <src>.tabc)
  run   <src>.ta [args...]      Compile and run (build to /tmp, then tavm)
  fmt   <file>.ta               Format .ta in place (token-level, gofmt-style)
  fmt   --check <file>.ta       Check formatting; exits 1 if file differs
```

Zero-install tinkering in the browser: [Playground](docs/playground.html) (WASM build — see `scripts/build-wasm.sh`).

## Project layout

| Path | Contents |
|------|----------|
| `src/` | The C VM: `vm.c` (bytecode execution), `scheduler.c` (scheduler / processes / mailboxes), `gc.c` (per-process semispace GC), `val.c` (NaN-boxing), `net.c` / `http.c` (network builtins) |
| `lib/` | TA compiler sources (`tokenizer.ta` / `parser.ta` / `typecheck.ta` / `codegen.ta` / `driver.ta`) + standard library + C modules (`http.c` / `demo.c`) + the build artifact `bootstrap.tabc` |
| `test/` | 7 test suites (basic / gc / actor / module / compiler / bootstrap / example) |
| `example/scripts/` | Runnable examples (echo server, KV server, http server, ...) |
| `docs/` | Design docs, language spec, Playground |
| `scripts/` | WASM build, lesson verifier (`verify-lessons.mjs`) |
| `benchmark/` | Performance benchmarks |

## Build & test

| Command | What it does |
|---------|--------------|
| `make` | Build `tavm` + C modules |
| `make test` | Run all 7 test suites (basic / gc / actor / module / compiler / bootstrap / example) |
| `make bootstrap` | Recompile the TA compiler with `lib/bootstrap.tabc`, writing the artifact back to `lib/bootstrap.tabc` |
| `make bootstrap-selfhost` | Verify the self-hosting fixed point: the artifact must be byte-identical to `bootstrap.tabc` |
| `make test-gc-asan` / `make test-gc-tsan` | GC tests under AddressSanitizer / ThreadSanitizer (also buildable with `ASAN=1` / `TSAN=1`) |
| `make fmt` / `make fmt-check` | Format C/C++ and `lib/*.ta` / verify formatting |
| `make benchmark` | Run performance benchmarks |

## Examples

The scripts in `example/scripts/` run directly:

```console
$ ./tinyactor run example/scripts/echo_server.ta    # TCP echo server
$ ./tinyactor run example/scripts/kv_server.ta      # simple KV server
```

`lib/serve.ta` is a static-file HTTP server (built on the `net` module, with one actor per connection):

```console
$ tavm lib/serve.ta 8080 /path/to/www
```

## Documentation

- [Design overview](docs/design.md)
- [TA language spec](docs/ta-language-spec.md)
- [Layered type model](docs/layered-type-model.md)
- [Generic ADT design](docs/generic-adt-design.md)
- [Design decisions](docs/design-decisions.md)
- [Writing C modules](docs/c-module.md)
- [Improvement plan](docs/improvement-plan.md)
- [Typecheck performance analysis](docs/typecheck-performance.md)

## Roadmap

supervisor → hot code reload → persistence → distribution → standard library → performance (P5) → DX (P6). Full plan in [ROADMAP.md](ROADMAP.md).

## FAQ (highlights)

**How is this different from Erlang / Go / Lua?**
Erlang is the actor model — TinyActor is a minimal Erlang (a few thousand lines of C VM) but leans harder into type safety at the language level. Go implements CSP; TinyActor implements actors. Like Lua it sits on top of C, but where Lua is designed to be embedded in C, TinyActor calls into C.

**Aren't languages with GC bad?**
GC dramatically lowers the mental burden of writing code. TinyActor takes the per-process route: no global stop-the-world, an actor's GC pause only affects that actor, and with preemptive fair scheduling the whole system can achieve good soft real-time behavior.

**Why not just use Lisp / Python syntax?**
TA's primary user is AI — and AI dislikes heavy parenthesis matching as much as it dislikes indentation as syntax.

The full FAQ lives on the homepage ([中文](docs/index.md) / [English](docs/index-en.md)).

## Project status

- Self-hosting fixed point verified (`bootstrap.tabc ≡ bootstrap_selfhost.tabc`)
- All 7 test suites pass, 0 failures
- Roadmap in progress — see [ROADMAP.md](ROADMAP.md)