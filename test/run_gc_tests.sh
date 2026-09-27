#!/bin/bash
# test/run_gc_tests.sh — run the GC tests
source "$(dirname "$0")/lib.sh"
# Run under GC stress: every allocation requests a collection, so the same
# cases exercise pointer pinning and C callback safety under worst-case timing.
export TA_GC_STRESS=1
TEST_TIMEOUT=$(( TEST_TIMEOUT < 300 ? 300 : TEST_TIMEOUT ))
run_category "GC (stress)" "$SCRIPT_DIR/gc"

print_summary
exit $([ $FAILED -eq 0 ])

print_summary
exit $([ $FAILED -eq 0 ])