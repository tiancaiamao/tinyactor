#!/bin/bash
# test/run_example_tests.sh — run the example/scripts self-contained tests.
#
# echo_server.ta and http_server.ta are persistent servers (run forever),
# so they are not executed here. Only the finite, self-contained tests
# (echo_test, concurrent_test) are run. echo_test is known-flaky on port
# 8091 in CI and is skipped by the shared skip list.
source "$(dirname "$0")/lib.sh"
EXAMPLE_DIR="$PROJECT_DIR/example/scripts"

echo -e "${BLUE}[Examples]${NC}"
run_test "$EXAMPLE_DIR/echo_test.ta"
run_build_run_test "$EXAMPLE_DIR/echo_test.ta"
run_test "$EXAMPLE_DIR/concurrent_test.ta"
run_build_run_test "$EXAMPLE_DIR/concurrent_test.ta"
run_test "$EXAMPLE_DIR/calc_server.ta"
run_build_run_test "$EXAMPLE_DIR/calc_server.ta"
echo ""
print_summary
exit $([ $FAILED -eq 0 ])