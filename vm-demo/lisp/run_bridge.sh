#!/bin/sh
# 端到端验证 TA AST -> lisp AST -> lispvm bytecode -> 实际运行值。
#
# bridge_test.ta 做三件事并落盘三份清单：
#   bridge.expect  正例 name path expected  —— 跑 lispvm，值不符即 FAIL
#   （stdout NEG 行）负例：不认识的东西必须在**编译期**被拒，而不是跑出错数
#   bridge.known   已知失败 name path expected —— 跑 lispvm，仍错记 XFAIL（不 gate）
#
# 判定：正例全过 + 负例全拒 → exit 0。已知失败不 gate，VM 修好后自动变 XPASS。
set -e
cd "$(dirname "$0")/../.."

LISPVM=${LISPVM:-/tmp/lispvm}
EXPECT=vm-demo/lisp/bridge.expect
KNOWN=vm-demo/lisp/bridge.known

[ -x "$LISPVM" ] || cc -O2 -o "$LISPVM" vm-demo/lisp/lispvm.c
ta_out=$(./tinyactor run vm-demo/lisp/bridge_test.ta 2>&1)

pass=0
fail=0
while read -r name path want; do
  got=$("$LISPVM" "$path" 2>&1 | tail -1)
  if [ "$got" = "$want" ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    echo "FAIL $name: want [$want] got [$got]"
  fi
done < "$EXPECT"

neg_pass=$(echo "$ta_out" | grep -c '^NEG OK' || true)
neg_fail=$(echo "$ta_out" | grep -c '^NEG FAIL' || true)

if [ -f "$KNOWN" ]; then
  while read -r name path want; do
    got=$("$LISPVM" "$path" 2>&1 | tail -1)
    if [ "$got" = "$want" ]; then
      echo "XPASS $name: 已修好 [$got] —— 可移进 bridge_test.ta 的 cases()"
    else
      echo "XFAIL $name: 仍错 [$got] —— lispvm let 槽缺陷，见 README 已知缺陷"
    fi
  done < "$KNOWN"
fi

echo "=== bridge: $pass passed, $fail failed"
echo "=== negative: $neg_pass rejected, $neg_fail wrongly accepted"
[ "$fail" -eq 0 ] && [ "$neg_fail" -eq 0 ]