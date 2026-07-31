#!/bin/bash
# test/run_compiler_tests.sh — run the compiler/parser/typecheck tests
source "$(dirname "$0")/lib.sh"
run_category "Compiler" "$SCRIPT_DIR/compiler"
print_summary
exit $([ $FAILED -eq 0 ])