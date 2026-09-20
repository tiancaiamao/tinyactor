#!/bin/bash
# test/run_gc_tests.sh — run the GC tests
source "$(dirname "$0")/lib.sh"
run_category "GC" "$SCRIPT_DIR/gc"

# Pin-discipline round: TA_GC_STRESS=1 requests a collection on *every*
# allocation, so a handler (or C module callback) that keeps a live heap
# reference in a C local across an allocation — instead of on the TA stack or
# behind the GC gate — surfaces here as a dangling pointer / wrong result.
export TA_GC_STRESS=1
run_category "GC (stress)" "$SCRIPT_DIR/gc"

print_summary
exit $([ $FAILED -eq 0 ])