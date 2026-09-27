#!/bin/bash
# Run the deliberately long GC stress cases outside the default test suite.
source "$(dirname "$0")/lib.sh"
export TA_GC_STRESS=1
TEST_TIMEOUT=$(( TEST_TIMEOUT < 300 ? 300 : TEST_TIMEOUT ))
run_test "$SCRIPT_DIR/gc/gc-deep-message.ta"
run_test "$SCRIPT_DIR/gc/gc-gcbench.ta"
print_summary
exit $([ $FAILED -eq 0 ])