#!/bin/bash
# test/run_gc_tests.sh — run the GC tests
source "$(dirname "$0")/lib.sh"
run_category "GC" "$SCRIPT_DIR/gc"

# Pin-discipline round: TA_GC_STRESS=1 requests a collection on *every*
# allocation, so a handler (or C module callback) that keeps a live heap
# reference in a C local across an allocation — instead of on the TA stack or
# behind the GC gate — surfaces here as a dangling pointer / wrong result.
export TA_GC_STRESS=1

# Collecting on every allocation makes each test run several times slower than
# the normal round — gc-deep-message.ta alone is ~33s locally and can blow
# past the default 60s budget on a loaded CI runner (issue #150 follow-up).
# Raise the per-attempt budget for this round only; the normal round above
# keeps the 60s default. Use a floor so a caller's larger TEST_TIMEOUT wins.
TEST_TIMEOUT=$(( TEST_TIMEOUT < 300 ? 300 : TEST_TIMEOUT ))
run_category "GC (stress)" "$SCRIPT_DIR/gc"

print_summary
exit $([ $FAILED -eq 0 ])