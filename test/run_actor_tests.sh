#!/bin/bash
# test/run_actor_tests.sh — run the actor/concurrency tests
source "$(dirname "$0")/lib.sh"
  run_category "Actor" "$SCRIPT_DIR/actor"
# Run the timer-granularity regression in the single-worker scheduler too.
# The generic actor suite uses the host default worker count.
run_test "$SCRIPT_DIR/actor/recv-after-granularity.ta" "NWORKERS=1"
# The timer/sleep acceptance in the single-worker scheduler too: poll-based
# sleep must overlap concurrent sleepers and not starve a busy actor.
run_test "$SCRIPT_DIR/actor/timer-sleep-concurrency.ta" "NWORKERS=1"
# Crash visibility tests (issue #28): assert stderr crash reports + exit
# codes, which the generic run_test harness can't check.
source "$SCRIPT_DIR/run_crash_tests.sh"
run_crash_tests
print_summary
exit $([ $FAILED -eq 0 ])