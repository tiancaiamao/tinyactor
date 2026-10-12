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
# Crash visibility tests (issue #28): assert stderr crash reports + exit
# codes, which the generic run_test harness can't check.
source "$SCRIPT_DIR/run_crash_tests.sh"
run_crash_tests
print_summary
exit $([ $FAILED -eq 0 ])