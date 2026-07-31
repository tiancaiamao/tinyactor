#!/bin/bash
# test/run_actor_tests.sh — run the actor/concurrency tests
source "$(dirname "$0")/lib.sh"
run_category "Actor" "$SCRIPT_DIR/actor"
print_summary
exit $([ $FAILED -eq 0 ])