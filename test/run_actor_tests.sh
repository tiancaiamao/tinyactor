#!/bin/bash
# test/run_actor_tests.sh — run the actor/concurrency tests
source "$(dirname "$0")/lib.sh"
run_category "Actor" "$SCRIPT_DIR/actor"
# Crash visibility tests (issue #28): assert stderr crash reports + exit
# codes, which the generic run_test harness can't check.
source "$SCRIPT_DIR/run_crash_tests.sh"
run_crash_tests
print_summary
exit $([ $FAILED -eq 0 ])