#!/bin/bash
# test/run_gc_tests.sh — run the GC tests
source "$(dirname "$0")/lib.sh"
# Run under GC stress: every allocation requests a collection, so the same
# cases exercise pointer pinning and C callback safety under worst-case timing.
export TA_GC_STRESS=1
# GC stress（每次分配都触发回收）下本套件断言共享 TA 堆的指针钉住与
# 回调安全（#249：stress 对 lisp 同样适用）。
SKIP_LIST="$SKIP_LIST gc-deep-message.ta gc-gcbench.ta"
TEST_TIMEOUT=$(( TEST_TIMEOUT < 300 ? 300 : TEST_TIMEOUT ))
run_category "GC (stress)" "$SCRIPT_DIR/gc"

print_summary
exit $([ $FAILED -eq 0 ])

print_summary
exit $([ $FAILED -eq 0 ])