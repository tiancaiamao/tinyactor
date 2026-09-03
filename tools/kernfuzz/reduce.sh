#!/bin/sh
# reduce.sh — thin forwarding entry for tools/kernfuzz/reduce.py
# (kernel-fuzzing §5.5: Python 主体，sh 只做参数转发，与 DEC-6 一致).
#
# Usage: ./reduce.sh <finding_dir | src.ta> --category <cat> [--out DIR]
exec python3 "$(dirname "$0")/reduce.py" "$@"