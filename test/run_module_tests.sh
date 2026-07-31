#!/bin/bash
# test/run_module_tests.sh — run the module system tests
source "$(dirname "$0")/lib.sh"
run_category "Module" "$SCRIPT_DIR/module"
print_summary
exit $([ $FAILED -eq 0 ])