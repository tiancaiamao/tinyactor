#!/bin/bash
# test/run_gc_tests.sh — run the GC tests
source "$(dirname "$0")/lib.sh"
# Run under GC stress: every allocation requests a collection, so the same
# cases exercise pointer pinning and C callback safety under worst-case timing.
export TA_GC_STRESS=1
# VM 选择：lib.sh 默认 --vm=lisp（7c-1 起唯一受支持的运行路径）。本套件在
# GC stress（每次分配都触发回收）下断言共享 TA 堆的指针钉住与回调安全
# （#249：stress 对 lisp 同样适用）。
SKIP_LIST="$SKIP_LIST gc-deep-message.ta gc-gcbench.ta"
TEST_TIMEOUT=$(( TEST_TIMEOUT < 300 ? 300 : TEST_TIMEOUT ))
run_category "GC (stress)" "$SCRIPT_DIR/gc"

print_summary
exit $([ $FAILED -eq 0 ])

print_summary
exit $([ $FAILED -eq 0 ])