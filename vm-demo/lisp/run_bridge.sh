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

LISPVM=${LISPVM:-./lispvm}
EXPECT=vm-demo/lisp/bridge.expect
KNOWN=vm-demo/lisp/bridge.known

# 总是重编：留着旧二进制会拿旧 VM 去验新字节码，「全过」是假的。
# （踩过：加了 OP_RESERVE 后仍报 25 个 bad opcode，就是因为复用了旧二进制。）
make lispvm
ta_out=$(./tinyactor run vm-demo/lisp/bridge_test.ta 2>&1)

pass=0
fail=0
while read -r name path want; do
  # 比**整段**输出而不是最后一行：print 不换行、println 换行，只看 tail -1
  # 的话 println 的那个换行根本测不到（它的值总在单独一行）。
  # 期望值里的 | 代表换行——expect 是行式清单，装不下真换行。
  got=$("$LISPVM" "$path" 2>&1)
  want=$(printf '%s' "$want" | awk '{gsub(/\|/, "\n"); print}')
  if [ "$got" = "$want" ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    echo "FAIL $name: want [$want] got [$got]"
  fi
done < "$EXPECT"

neg_pass=$(echo "$ta_out" | grep -c '^NEG OK' || true)
neg_fail=$(echo "$ta_out" | grep -c '^NEG FAIL' || true)

# -q：关掉 entry 值打印后应只剩 print 的输出（对拍 TA runtime 时要用）。
q_pass=0
q_fail=0
if [ "$("$LISPVM" -q vm-demo/lisp/bridge_cfunc-print.bc 2>&1)" = "42" ]; then
  q_pass=1
else
  q_fail=1
  echo "QUIET FAIL: -q 应只留 print 的输出 42，实际 [$("$LISPVM" -q vm-demo/lisp/bridge_cfunc-print.bc 2>&1)]"
fi

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
echo "=== quiet: $q_pass passed, $q_fail failed"
[ "$fail" -eq 0 ] && [ "$neg_fail" -eq 0 ] && [ "$q_fail" -eq 0 ]