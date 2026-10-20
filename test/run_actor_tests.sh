#!/bin/bash
# test/run_actor_tests.sh — run the actor/concurrency tests
source "$(dirname "$0")/lib.sh"
  # The 1M actor resource gate runs separately on Linux; it must not be part
# of the ordinary actor suite or compete with the other test categories.
SKIP_LIST="$SKIP_LIST million-actors-ready.ta"
run_category "Actor" "$SCRIPT_DIR/actor"
# Run the timer-granularity regression in the single-worker scheduler too.
# The generic actor suite uses the host default worker count.
run_test "$SCRIPT_DIR/actor/recv-after-granularity.ta" "NWORKERS=1"
# The timer/sleep acceptance in the single-worker scheduler too: poll-based
# sleep must overlap concurrent sleepers and not starve a busy actor.
run_test "$SCRIPT_DIR/actor/timer-sleep-concurrency.ta" "NWORKERS=1"
# Mirror of the two lines above: the worker-pool path (NWORKERS>1) is not the
# default, so pin it here as well — the multi-thread scheduling tests and the
# preemption test must hold with the pool on. Without these the pool would only
# ever run when someone exports NWORKERS by hand (mt-revival.md §6 D4).
run_test "$SCRIPT_DIR/actor/multithread-basic.ta" "NWORKERS=2"
run_test "$SCRIPT_DIR/actor/multithread-msg.ta" "NWORKERS=2"
run_test "$SCRIPT_DIR/actor/preempt.ta" "NWORKERS=8"
# The buffer module's handle table is global, so its regression needs the
# pool: with one worker the actors never interleave and the race cannot
# happen. 8 workers, 8 actors.
run_test "$SCRIPT_DIR/actor/multithread-buffer.ta" "NWORKERS=8"
# The process module's handle table is global (lib/process.c): with the
# pool on, two workers spawning at the same time could land on the same
# slot — one actor's wait then reaped the other actor's child and the
# borrowed pipe fds got closed twice (a reused fd number killed another
# actor's pipe). The test spawns tagged children from 8 actors and
# checks every child answered with its own tag.
run_test "$SCRIPT_DIR/actor/multithread-process.ta" "NWORKERS=8"
# The symbol table is shared VM state (src/api.c vm_intern_symbol): with the
# pool on, one worker interns a name while another reads the table back
# (str.sym_to_str / print) — the reader must not see the array and the count
# from different moments. The test interns from 8 actors and checks every
# name reads back unchanged.
run_test "$SCRIPT_DIR/actor/multithread-symbol.ta" "NWORKERS=8"
# Pool-path correctness gate (mt-revival.md §6 D5): K CPU-bound actors fan in
# to main; the test asserts exactly-once results and per-process state
# isolation — the invariants a broken pool breaks, which a single worker
# cannot even reach (it never interleaves two actors). Two widths straddle
# the 16 actors it fans in.
run_test "$SCRIPT_DIR/actor/multithread-fanin.ta" "NWORKERS=2"
run_test "$SCRIPT_DIR/actor/multithread-fanin.ta" "NWORKERS=4"
# The sha256 module has no shared state (#398), so its actor tests pass on the
# single-worker default — but then no two actors ever run on two threads, and
# a regression that puts a shared context or a lock back into lib/sha256.c
# would still pass the suite. Pin the pool so the threaded path is gated:
# 8 workers, 8 actors hashing / finalising concurrently.
run_test "$SCRIPT_DIR/actor/sha256-parallel.ta" "NWORKERS=8"
run_test "$SCRIPT_DIR/actor/sha256-copy-across-actors.ta" "NWORKERS=8"

# Crash visibility tests (issue #28): assert stderr crash reports + exit
# codes, which the generic run_test harness can't check.
source "$SCRIPT_DIR/run_crash_tests.sh"
run_crash_tests
print_summary
exit $([ $FAILED -eq 0 ])