#!/bin/bash
# test/run_basic_tests.sh — run the basic language tests
source "$(dirname "$0")/lib.sh"
run_category "Basic" "$SCRIPT_DIR/basic"
print_summary
exit $([ $FAILED -eq 0 ])