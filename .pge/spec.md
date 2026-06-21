# Spec: Fix Test Framework + VM/GC Bugs

## Goal
Fix 5 bugs that cause false PASS in the test suite: test framework masks segfaults/timeouts, GC corrupts stack on buffer growth, stall counter kills long computations, undefined function calls cause segfault, bytes operations cause segfault.

## Acceptance Criteria

### L1 — Structural
- [ ] `make` compiles cleanly — Verify: `cd /Users/genius/project/tinyactor && make 2>&1 | grep -c error` returns 0
- [ ] run_all_tests.sh treats exit 139 and 124 as FAIL — Verify: `grep -c '139\|124' run_all_tests.sh` in fail-check context
- [ ] run_all_tests.sh checks for empty output — Verify: `grep -c 'empty' run_all_tests.sh`

### L2 — Behavioral
- [ ] gc-deep-list.lisp outputs correct result — Verify: `./ta test/scripts/gc-deep-list.lisp` outputs expected number
- [ ] fib.lisp outputs correct result (no silent failure at fib≥28) — Verify: `./ta test/scripts/fib.lisp` outputs 832040
- [ ] tail-call-deep.lisp outputs correct result — Verify: `./ta test/scripts/tail-call-deep.lisp` outputs expected
- [ ] Calling undefined function prints error instead of segfault — Verify: `./ta test/scripts/module-basic.ta` exits with non-zero, non-139 code
- [ ] bytes-basic.lisp exits gracefully (not segfault) — Verify: exit code is not 139
- [ ] Previously passing tests still pass — Verify: `cd test && bash run_all_tests.sh` shows no regressions in true-PASS tests

## Constraints
- Do NOT break existing passing tests
- Do NOT change the NaN-boxing value representation
- Keep the GC semi-space copying algorithm
- Files to modify: src/gc.c, src/vm.c, src/compile.c (if needed), test/run_all_tests.sh

## Out of Scope
- Implementing full bytes type support
- Implementing module system (math/tokenizer/parser modules)
- Implementing selective receive