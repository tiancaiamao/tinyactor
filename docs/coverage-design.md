# Coverage — design note (TA program coverage)

Status: phase 1 shipped (`tinyactor build --cov`). Function-level aggregation is available via `make coverage-ta` and CI gates it at 85%; finer granularity (phases 2–3) remains planned.


## Problem

Two coverages, deliberately kept separate:

1. **Implementation coverage** — C lines of `tavm` exercised by tests.
   Measured with llvm-cov (`make coverage`, gated by `COV_MIN`). This says
   "the implementation is tested", not "TA programs are measurable".
2. **Program coverage** — which parts of a *TA program* actually ran.
   This note is about #2.

## Prior art (survey conclusion)

| System | Mechanism | Lesson |
|---|---|---|
| Erlang `cover` | compile-time instrumentation, ETS counter sink | instrumentation is a *compilation mode*, sink is a VM-side table (message passing is a lossy counter sink) |
| Go `-cover` | same school: toolchain rewrites expressions, atomic counters | artifact carries opaque ids; mapping lives in a side table |
| SBCL `sb-cover` | an `optimize` quality — the compiler weaves counters; 3-valued (never / fully / partially evaluated) | no second tool needed when the compiler already holds form structure |
| cloverage / racket cover | source-tree / syntax-object rewrite | homoiconicity makes instrumentation a tree walk |
| Guile | VM ip tracing via bytecode ip→source tables | only viable when line tables already exist in the artifact |
| luacov | runtime debug hook | what you're stuck with when neither compiler nor artifact knows positions |

**Rule: the mechanism lands where source-position knowledge lives.**
The Lisp family adds: granularity is naturally the *form/expression*, not
the line, and "entered but not completed" is the valuable third state.

## TinyActor facts that decided the design

- TABC has **no line tables** → Guile/V8/Ruby routes are out (format bump,
  VM changes — the wrong layer per the layering principle).
- The AST **is data** (S-expression shape) and survives until codegen →
  instrumentation is a plain AST→AST tree walk (cloverage school).
- The typechecker resolves unknown qualified calls on registered C
  modules permissively, and codegen compiles them as `OP_CCALL_NAME` →
  an injected `(cov.hit k)` needs **no typecheck, codegen or VM change**.
- `resolve_loop` treats `import <registered-name>` as builtin and does
  **not** load a TA module of that name — so the instrumenter module must
  not collide with the C module name (TA side: `covinst`; C side: `cov`).
  This collision silently produced `nil` from every instrument call; it
  is why the two modules have different names.

## Phase 1 (shipped): function-entry coverage

`tinyactor build --cov prog.ta prog.tabc`:

1. after typecheck, before codegen, `lib/bootstrap/covinst.ta` walks the
   resolved forms and rewrites every `(define (name . params) body)` to
   `(define (name . params) (begin (cov.hit k) body))`, returning the
   `k -> name` table;
2. the driver writes `prog.tabc.covmap` (one `k name` line per fn);
3. at runtime `src/cov.c` (registered module `cov`, always present,
   zero cost when unused) accumulates counts in a mutex-protected
   growable array shared by all actors;
4. `cov.dump(path)` writes `id count` lines; the report is the join of
   covmap × dump.

Numbering is assigned over the post-resolve form list — the same list
codegen compiles — so ids and the covmap agree by construction.

Untouched: parser, AST shape, TABC format, typecheck, codegen, VM.

## Phase 2 (planned): statement / match-arm granularity + lines

The parser threads a token cursor everywhere but drops it; `tokenize_pos`
already yields parallel `(line col off)` per token, and driver error
reporting already maps tokpos → line. Phase 2 makes the parser emit a
*parallel* span list (start tokpos per statement / match arm — no AST
shape change, the `tokenize_pos` design), and covmap lines become
`k file:line name`. Ids must then be allocated across imported modules
(per-module segments), which phase 1 deliberately avoids by numbering
after resolve within one image.

## Phase 3 (planned): the SBCL third state

Arms of a `match` (and `if` branches) get an *entered-but-not-taken*
state instead of binary hit/miss — a reporting distinction; the sink
grows a second counter per id.