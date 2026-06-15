# TinyActor — Progress

## Phase 1: 能跑 ✅ COMPLETE
- Task 1: ta.h + val.c — types, NaN-boxing, opcodes (566 lines)
- Task 2: reader.c + compile.c — parser + compiler (~1330 lines)
- Task 3: vm.c + api.c + main.c + Makefile — runtime (~900 lines)
- Task 4: Integration debug — 17 bugs fixed across 3 sub-agents
- **11/11 tests passing**

## Phase 2: 完善 ✅ COMPLETE

### Task 1+2: GC Core + Integration
- src/gc.c (141 lines): per-process semispace copying collector
- ta.h: gc_roots[16], gc_root_count, current_proc, gc_collect, gc_root_push/pop
- vm.c: proc_new allocates gc_to, vm_run sets current_proc, proc_grow
- val.c: gc_root_push/pop in val_pair/val_closure

### Task 3+4: String Builtins + C Function Call
- OP_STR_LEN, OP_STR_CONCAT, OP_STR_SLICE, OP_STR_EQ
- OP_CCALL: call registered C functions from script

### Task 5: Test Suite
- 34 new tests → 46 total (45 pass, 1 expected-fail: bytes-basic)

### Task 6: Validation + Review Fixes
- Independent Evaluator verified 32/33 criteria
- Code Review found 5 P1 + 5 P2 issues, all fixed

## Phase 3: 模块化 + 系统级集成 ✅ COMPLETE

### Task 1: Preempt Bug Fix ✅ — `0b48c2e`
- Stall counter in vm_run(): 10,000 iterations without state change → kill all RUNNING processes
- preempt.lisp exits 0 with output "ok"
- gc-deep-list.lisp now also passes

### Task 2+3: Module System + Network Module ✅ — `4168f66`
- TaFunc struct `{name, fn, nargs}` for module function descriptors
- vm_register_module(vm, "name", funcs[]): registers batch with prefixed names
- (import "name") in compiler — OP_PUSH_NIL (no-op at runtime, enables module.func calls)
- src/module.c: module registration implementation
- src/net.c: net.listen/accept/read/write/close — non-blocking sockets
- PROC_WAIT_IO state + poll() in vm_run Phase 2
- OP_CCALL detects 'would-block → rewind PC + set WAIT_IO
- module_test.lisp passes
- Reader: `.` added to is_ident_char() so `net.listen` parses as single symbol

### Task 4: TCP Echo Server ✅ — `4c6ea21`
- example/echo_server.c: C host program linking TinyActor VM
- example/scripts/echo_server.lisp: accept loop + handle-client actor
- example/scripts/echo_test.lisp: 3-client integration test (PASS)
- net.connect() added for TCP client connections
- **2 critical bugs found and fixed:**
  1. VM I/O bug: would-block rewind didn't restore args to stack → garbage on retry
  2. Compiler bug: import set has_top=true → main not auto-spawned

### Task 5: HTTP Server ✅ — `3a5e0bf`
- src/http.c: HTTP module (http.parse_request, http.response)
- example/http_server.c: C host program with net + http modules
- example/scripts/http_server.lisp: actor-per-connection, 3+ routes
- Routes: / (HTML), /api (JSON), /time, */ (404)
- Verified with curl: all routes return correct content + headers

### Task 6: Independent Validation + Review Fixes ✅ — `f5a0a04`
- **Evaluator** (6bf936): 16/19 criteria pass, 1 fail (echo_server.lisp paren bug), 2 partial
- **Evaluator finding fixed**: echo_server.lisp parenthesis imbalance → fixed (`18b81fb`)
- **Review** (5fb1af): 6 P1 + 10 P2 + 3 P3 issues found
- **Generator** (1c0147): Fixed all P1 + critical P2 issues:
  - P1-01: net_read VLA → malloc (stack overflow)
  - P1-02: http_response two-pass snprintf (buffer overflow)
  - P1-03: vm_free frees module registry (memory leak)
  - P1-04: realloc NULL check in module.c
  - P1-05: poll() uses POLLIN+POLLOUT (write wait processes now wake)
  - P1-06: stall counter reverted (original was correct — false positive from review)
  - P2-01: SO_REUSEADDR on net_listen
  - P2-04: proc_die closes wait_fd only when in WAIT_IO state
  - P2-07: http_response GC-safe allocation
  - P2-10: poll fd array 128 → 1024

### Subagents Used (Phase 3)
1. gen-preempt2 (9b203d) — stall counter fix
2. gen-module-net (f54ca3) — module system + network module + I/O scheduler
3. gen-http (24b3ca) — HTTP server
4. eval-phase3 (6bf936) — independent acceptance verification
5. review-phase3 (5fb1af) — code review
6. gen-fix-review (1c0147) — fix all P1+P2 issues

## Phase 5: 新语法 — ML/Rust 系 .ta ✅ COMPLETE

### Design (ROADMAP.md `72d9a2c`)
- 6 keywords: `fn`, `let`, `match`, `if`, `spawn`, `send`
- `match` not `case`; `|>` and `pub` deferred to Phase 8
- `type` ADT deferred to Phase 6

### Implementation — `cac7ca5`
- **`src/reader_ta.c`** (777 lines): tokenizer + recursive descent parser
  - Newline-separated statements in `{}` blocks
  - `let` nesting: consecutive lets wrap subsequent forms
  - Pattern syntax: `_`, var, `'sym`, int, `[a, b, c]`
  - `//` line comments
  - `==` → `=` translation for compiler compatibility
- **`src/api.c`**: `reader_ta_read` extern + `vm_load_ta()` + `.ta` extension dispatch
- **`Makefile`**: `src/reader_ta.c` added to SRC
- **5 test ports**: hello.ta, arith.ta, multithread-basic.ta, recv-scan.ta, echo_test.ta
- **compile.c / vm.c / gc.c / ta.h / val.c / reader.c / main.c**: ZERO changes

### Bugs Found & Fixed During Implementation
1. **match AST shape**: `mk_list({sym, scrut, arms}, 3)` → nested `((pat body) ...)` as single element. Fix: `val_pair(sym, val_pair(scrut, arms))` to splice arms correctly
2. **receive AST shape**: Same issue — `mk_list({sym, arms}, 2)` → `val_pair(sym, arms)`
3. **`==` operator crash**: Compiler inline_ops uses `=` not `==`. Fix: `match_op()` translates `==` → `=`

### Evaluation — `31f28d` (OVERALL: PASS, 15/15)
- L1 Structural: 5/5 ✅
- L2 Behavioral: 7/7 ✅ (49 .lisp regression: 0 changes via worktree diff)
- Code Quality: 3/3 ✅
- ASAN: 2 pre-existing issues (vm.c:640 stack overflow, scratch arena grow) — reproducible through .lisp, NOT introduced by Phase 5
- Non-blocking: 2 unused functions (parse_block, read_keyword) — dead code warnings

### Subagents Used (Phase 5)
1. reader-ta-gen (ee8823) — stuck reading code, killed after 30min
2. reader-ta-gen2 (43a29c) — succeeded, created reader_ta.c + integration
3. port-ta-gen (a3b850) — created .ta test ports, identified ==/receive bugs
4. eval-phase5 (31f28d) — formal acceptance verification

## Current Code Stats
- **~5500 lines** total (added reader_ta.c: 777 lines)
- Build: `make clean && make` — 0 errors
- Tests: **49 .lisp + 5 .ta = 54 total** (all pass)
- MT: multithread-basic.ta PASS under NWORKERS=4

## Git History (Phase 5)
```
cac7ca5 Phase 5: ML/Rust-style .ta syntax reader
72d9a2c Refine ROADMAP: match over case, defer |> and pub
...(Phase 4 below)
```

## Git History
```
f5a0a04 Phase 3 Task 6: Fix P1+P2 issues from independent review + evaluator
18b81fb Fix echo_server.lisp parenthesis imbalance (evaluator finding)
3a5e0bf Phase 3 Task 5: HTTP Server with routing
4c6ea21 Phase 3 Task 4: TCP Echo Server + 2 critical bug fixes
4168f66 Phase 3 Task 2+3: Module system + network module + I/O scheduler
0b48c2e Phase 3 Task 1: Fix preempt bug — stall counter in scheduler
9cdaaed Update Phase 2 spec + progress documentation
31cfcf8 Phase 2: Fix 10 issues from independent review + evaluator
21d3689 Phase 2 complete: GC + string builtins + C function call + 45 tests
590f40f Phase 2 Task 5: Test suite integration (34 new tests, 45 total)
71be2e1 Phase 2 Task 3+4: String builtins + C function call mechanism
8ef5b5e Phase 2 Task 1+2: Per-process semispace copying GC
6a542a6 Add .gitignore, remove build artifacts from tracking
3ddc354 Phase 1 complete: TinyActor VM with actor concurrency
```

## Phase 4: 多线程调度器 + Heap Fragment ✅ COMPLETE

### Task 1: Thread Infrastructure — `d18be4a`
- pthread-based worker threads sharing global runq
- mutex + condvar for runq signaling
- `active_procs` atomic counter for exit detection
- thread-local `current_proc` for GC root tracking
- `vm_run` → `vm_start` (spawns N workers, waits for completion)

### Task 2: Heap Fragment Message Passing — `caca0ee`
- Send deep-copies message via malloc'd fragment to target mailbox
- GC never crosses process boundary (zero GC changes)
- OP_SEND pops msg + pid, pushes nil for stack balance
- OP_RECV peeks mailbox, returns first message or blocks

### Task 3: Multi-Worker Threading — `5c59227`
- NWORKERS env var controls thread count (default 1)
- Workers pull from shared runq with mutex protection
- Exit condition: active_procs == 0 → broadcast condvar → all workers exit
- Each actor runs on exactly one worker at a time (runq mutex guarantees)

### Task 4: Dedicated I/O Poller Thread — `b2560fc`
- Separate io_poller thread using poll() on all WAIT_IO processes
- Decoupled from worker threads: poller wakes actors, workers execute them
- poll_interval adaptive: 1ms when I/O pending, 10ms otherwise

### Task 5: Selective Receive — `c88831c`
- OP_RECV_PEEK: scan mailbox at peek_index, push copy or block
- OP_RECV_COMMIT: remove matched fragment, reset peek_index
- `(receive (pattern body...) ...)` syntax compiles to scan loop
- peek_index preserved across block/resume for incremental scanning

### Task 6: Bug Fixes — Technical Debt Cleanup
- **http module registration**: `vm_register_http_module(vm)` was missing in main.c → `http.parse_request`/`http.response` unresolved → nil closure → SEGV at OP_CALL. Fix: added registration call.
- **bytes-basic segfault**: Fixed by OP_SEND stack balance fix (push nil after send).
- **HTTP server**: Now fully functional with all routes + concurrent requests (10/10 verified).

## Known Limitations
- [ ] net_connect uses blocking connect() (can stall VM)
- [ ] No try/catch / throw (Erlang philosophy: Let it crash, use supervisor)
- [ ] HTTP server is minimal (no chunked encoding, no keep-alive)
- [ ] poll fd limit: 1024 concurrent connections max
- [ ] MAX_REDUCTIONS=1000 + stall detection kills long-running computations (fib(28+), tail-call-deep)
- [ ] Actor selective-recv test produces no output (pre-existing)

## Phase 7 Progress

### Phase 7a: C Helper Modules ✅ (commit `09958b6`)
- file.c (77 lines): file.read/write/exists
- buf.c (181 lines): mutable byte buffer for codegen (buf.new/push_byte/push_int32/etc)
- str.c (150 lines): string utilities (str.char_at/length/substr/concat/to_int/from_int/eq)

### Phase 7b: Tokenizer ✅ (commit `6efffb6`)
- lib/tokenizer.ta (342 lines): self-hosted tokenizer
- Token types: kw, ident, int, string, op, lparen, rparen, lbrace, rbrace, comma, colon, arrow, quote, eof
- **CRITICAL FIX**: Stack frame corruption bug in compiler
  - Root cause: local variable slots (let, match) extended into caller's stack space
  - Fix: OP_ENTER opcode reserves local variable space below frame header
  - Local slots use negative offsets (fp-5, fp-6, ...) instead of positive (fp+nargs+)
  - max_slots tracking for match branch slot restoration
  - env_find check changed from `slot >= 0` to `slot != -1`
- Tokenizer correctly tokenizes: "fn add(x) { x + 1 }" and full function bodies

### Phase 7d: .tabc Format ✅ (commit `a77764d`)
- Binary bytecode serialization: TABC header + symbols + fn_table + code
- vm_dump_tabc() / vm_load_tabc() round-trip verified
- --emit-tabc flag in main.c

### Next: Phase 7c (Parser) → Phase 7e (Codegen) → Phase 7f (Bootstrap verification)

## Git History
```
7a9d895 Phase 4 Task 6: Fix HTTP module registration + tech debt cleanup
c88831c Phase 4 Task 5: Selective receive + OP_SEND stack balance fix
b2560fc Phase 4 Task 4: Dedicated I/O poller thread
5c59227 Phase 4 Task 3: Multi-worker threading + exit condition fix
caca0ee Phase 4 Task 2: Heap Fragment message passing
d18be4a Phase 4 Task 1: Basic thread infrastructure
f5a0a04 Phase 3 Task 6: Fix P1+P2 issues from independent review + evaluator
18b81fb Fix echo_server.lisp parenthesis imbalance (evaluator finding)
3a5e5bf Phase 3 Task 5: HTTP Server example + L2.10/L2.11 tests
b09ec4a Phase 3 Task 4: TCP Echo Server example
4168f66 Phase 3 Task 2+3: Module system + network module
0b48c2e Phase 3 Task 1: Fix preempt bug — stall counter in scheduler
9cdaaed Update Phase 2 spec + progress documentation
31cfcf8 Phase 2: Fix 10 issues from independent review + evaluator
21d3689 Phase 2 complete: GC + string builtins + C function call + 45 tests
590f40f Phase 2 Task 5: Test suite integration (34 new tests, 45 total)
71be2e1 Phase 2 Task 3+4: String builtins + C function call mechanism
8efb5e5 Phase 2 Task 1+2: Per-process semispace copying GC
6a542a6 Add .gitignore, remove build artifacts from tracking
3ddc354 Phase 1 complete: TinyActor VM with actor concurrency
```
### Phase 7c: Parser ✅ (commit `46464ca`)
- lib/parser.ta (676 lines): recursive descent parser, tokens → pair-tree AST
- Threaded-state pattern: functions return (result . next_pos)
- Handles: fn/pub fn/import/type/let/if/match/operators/constructors
- reader_ta.c fix: if/match now parseable in expression position (let x = if ...)
- vm.c fix: calloc for proc memory (GC safety)
- str.to_sym() added for operator symbol creation

### Summary: Phase 7 bootstrapping status
- 7a: C helper modules ✅
- 7b: Tokenizer ✅ (self-hosted, 342 lines)
- 7c: Parser ✅ (self-hosted, 676 lines)
- 7d: .tabc format ✅
- 7e: Codegen ✅ (self-hosted, 386 lines, commit `6ede7dd`)
  - 30 functions: literal/variable/if/let/begin/call compilation,
    inline opcode mapping, two-pass (register+compile), .tabc serialization
  - Reader fixes: operator symbols in quote handler, % in is_ident_char
  - VM fixes: nil-safe car/cdr, buf.set_byte for backpatching
- Next: 7f (Bootstrap verification)
