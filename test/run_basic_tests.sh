#!/bin/bash
# test/run_basic_tests.sh — run the basic language tests
source "$(dirname "$0")/lib.sh"
run_category "Basic" "$SCRIPT_DIR/basic"
# TLS handshake happy path: lib/tls.ta's env-gated scenario 4 has no
# in-TA server, so run_tls_tests.sh supplies one via openssl s_server.
source "$(dirname "$0")/run_tls_tests.sh"
run_tls_tests
print_summary
exit $([ $FAILED -eq 0 ])