# Coverage — design note (TA program coverage)

> 状态：本工具链曾随 PR #275 删除，现已恢复（`tinyactor build --cov`、`make coverage-ta`、`lib/bootstrap/covinst.ta`、`tools/coverage_html.ta` 等均在位）。TA 函数入口覆盖率门 = compiler 组（插桩版 driver）与 program 组（各测试程序自己的插桩镜像）按源码位置合并后的函数覆盖率 ≥ `COV_TA_MIN`；C 实现的 line coverage ≥ `COV_MIN`（`make coverage`）是另一条独立门。

Status: phases 1–2 shipped (`tinyactor build --cov`, restored — see the
note above); phase 3 remains planned.


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
3. at runtime `lib/cov.c` (lazy dylib module `cov`, dlopened on first
   `cov.*` reference, zero cost when unused) accumulates counts in a
   mutex-protected growable array shared by all actors;
4. `cov.dump(path)` writes `id count` lines; the report is the join of
   covmap × dump. `lib/cov.c` also registers an `atexit` hook: an
   instrumented image that exits with `TA_COV_DUMP_DIR` set writes
   `<dir>/cov-<pid>.cov` by itself — no injected call, no VM flag, so
   *any* TA program (the compiler included) is measurable.

Numbering is assigned over the post-resolve form list — the same list
codegen compiles — so ids and the covmap agree by construction.

Ids are image-local, so a dump is only meaningful paired with the covmap
of the image that wrote it. `make coverage-ta` therefore keeps two groups
and merges them by source position (`tools/merge_coverage_ta.py`): the
compiler group is one instrumented driver (`TA_DRIVER_BC`) compiling the
whole suite, the program group is each test program's own instrumented
image; `tinyactor` files the pairs under `TA_COV_MAP_DIR/{maps,dumps}/<tag>/`.

Untouched: parser, AST shape, TABC format, typecheck, codegen, VM.

## Phase 2 (shipped): statement / match-arm granularity + lines

Implemented in `lib/bootstrap/parser.ta` behind `--cov` (see
`docs/ta-coverage-phase2-plan.md` for the full rationale):

* `tokenize_pos` returns a `(tokvec poss)` bundle; the parser threads it as
  `toks` and unwraps via accessors, so signatures don't change.
* Under `--cov` the parser wraps every statement body and single-expression
  match arm as `(begin (cov.line path line) form)` — a *permissive
  statement* that is invisible to typecheck, resolve and codegen (an
  unbound symbol in a builtin-module call position; `cov.line` is not
  registered in the C `cov` module, so a marker that survives
  instrumentation fails loudly at runtime rather than reporting
  nothing). `covinst` walks every top-level form (defines and
  const-bound lambdas alike), not just defines.
* `covinst.ta` rewrites the tags generically: `(begin (cov.line f l) s)` →
  `(begin (cov.hit k) s)`, one new id per marker, ids allocated across the
  whole post-resolve form list as before. covmap rows become
  `k file:line name` (file taken from the first statement's tag, `:0` entry
  rows are the phase-1 function entries); the report gains a second header
  line `TA statement coverage: X/Y` and per-statement rows sorted by
  file/line.

Untouched: TABC format, typecheck, codegen, VM. Cache: tagged AST hashes
differ → cache miss only, no correctness impact.

Viewing: `make coverage-ta` rendered
`coverage/ta/coverage.html` — a
go-tool-cover style annotated source view (green = executed statement
line, red = never executed), generated by the TA program
`tools/coverage_html.ta` straight from the covmap + dumps. The same run
emitted `coverage/ta/misses.txt` — a plain-text, one-location-per-line miss
list (`fn <path> <name>` / `line <path>:<line>`) meant for agents/CI to
grep, avoiding any need to parse the HTML.

## Phase 3 (planned): the SBCL third state

Arms of a `match` (and `if` branches) get an *entered-but-not-taken*
state instead of binary hit/miss — a reporting distinction; the sink
grows a second counter per id.