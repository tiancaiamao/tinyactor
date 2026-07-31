#!/bin/bash
# test/run_gc_tests.sh — run the GC tests
source "$(dirname "$0")/lib.sh"
run_category "GC" "$SCRIPT_DIR/gc"
print_summary
exit $([ $FAILED -eq 0 ])