#!/bin/bash
# test/run_bootstrap_tests.sh — bootstrap fixed-point + self-hosting tests.
#
# Rebuild lib/bootstrap.tabc from lib/bootstrap/driver.ta ONCE, then verify:
#   1. fixed point: the rebuild is bit-identical to the committed bootstrap
#   2. self-hosting: the rebuilt compiler can compile+run hello.ta
#
# Note: rebuilding driver.ta runs the full typecheck over lib/ and takes
# a couple of minutes, so the two checks share a single rebuild.
source "$(dirname "$0")/lib.sh"

# The rebuild step runs full typecheck over all of lib/ (~5.5k lines).
# Optional SKIP_BOOTSTRAP=1 skips this category (no longer needed — typecheck
# perf fix made the rebuild ~12s — but kept for local debugging convenience).
if [ "${SKIP_BOOTSTRAP:-0}" = "1" ]; then
  echo -e "${YELLOW}[Bootstrap] SKIPPED (SKIP_BOOTSTRAP=1)${NC}"
  echo ""
  exit 0
fi

run_bootstrap_tests() {
  # Single rebuild shared by both checks below
  local rebuilt="/tmp/fp_$$.tabc"
  local log="/tmp/fp_$$.log"
  local t0=$SECONDS
  timeout 300 bash -c "cd '$PROJECT_DIR' && '$TINYACTOR' build lib/bootstrap/driver.ta '$rebuilt'" >"$log" 2>&1
  local build_exit=$?
  local rebuild_secs=$((SECONDS - t0))

  # 1. Fixed point: rebuilt must be bit-identical to the committed bootstrap
  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "bootstrap fixed point:"
  if [ $build_exit -ne 0 ]; then
    echo -e "${RED}❌ FAIL${NC} (rebuild failed in ${rebuild_secs}s)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("bootstrap fixed point (rebuild failed)")
  elif cmp -s "$rebuilt" "$BOOTSTRAP"; then
    echo -e "${GREEN}✅ PASS${NC} (bit-identical, rebuild ${rebuild_secs}s)"
    PASSED=$((PASSED + 1))
  else
    echo -e "${RED}❌ FAIL${NC} (mismatch, rebuild ${rebuild_secs}s)"
    python3 - "$BOOTSTRAP" "$rebuilt" <<'PYDIAG'
import sys

reference, rebuilt = sys.argv[1:]
a = open(reference, "rb").read()
b = open(rebuilt, "rb").read()
limit = min(len(a), len(b))
first = next((i for i in range(limit) if a[i] != b[i]), limit)
diff_count = sum(x != y for x, y in zip(a, b)) + abs(len(a) - len(b))
print(f"    bootstrap diagnostic: sizes {len(a)} vs {len(b)}, differing bytes {diff_count}")
print(f"    first difference: offset {first} (0x{first:x})")
lo = max(0, first - 16)
hi = min(max(len(a), len(b)), first + 16)
print(f"    reference[{lo}:{hi}]: {a[lo:hi].hex()}")
print(f"    rebuilt  [{lo}:{hi}]: {b[lo:hi].hex()}")

def u32(data, off):
    return int.from_bytes(data[off:off + 4], "little")

if len(a) >= 24 and a[:4] == b"TABC" and b[:4] == b"TABC":
    n_symbols = u32(a, 8)
    n_fns = u32(a, 12)
    pos = 24
    for _ in range(n_symbols):
        pos += 4 + u32(a, pos)
    syms_end = pos
    fn_table_end = syms_end + 4 * n_fns
    names_end = fn_table_end
    if u32(a, 4) >= 2:
        for _ in range(n_fns):
            names_end += 4 + u32(a, names_end)
    sections = (("header", 0, 24), ("symbols", 24, syms_end),
                ("fn_table", syms_end, fn_table_end),
                ("fn_names", fn_table_end, names_end),
                ("code", names_end, len(a)))
    for name, start, end in sections:
        if start <= first < end or (first == len(a) and first == end):
            print(f"    section: {name} [{start}, {end})")
            break
PYDIAG
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("bootstrap fixed point (mismatch)")
  fi

  # 2. Self-hosting: use the rebuilt compiler to compile+run hello.ta
  local sh_hello="/tmp/sh_hello_$$.tabc"
  local log2="/tmp/sh2_$$.log"
  local log3="/tmp/sh3_$$.log"
  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "self-hosting:"
  if [ $build_exit -ne 0 ]; then
    echo -e "${RED}❌ FAIL${NC} (rebuild failed)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("self-hosting (rebuild failed)")
  else
    timeout 15 bash -c "cd '$PROJECT_DIR' && '$TAVM_BIN' '$rebuilt' test/basic/hello.ta '$sh_hello'" >"$log2" 2>&1
    if [ $? -ne 0 ]; then
      echo -e "${RED}❌ FAIL${NC} (rebuilt compiler can't compile)"
      FAILED=$((FAILED + 1))
      FAILED_TESTS+=("self-hosting (compile failed)")
    else
      timeout 3 bash -c "cd '$PROJECT_DIR' && '$TAVM_BIN' '$sh_hello'" >"$log3" 2>&1
      local run_exit=$?
      local run_output=$(head -1 "$log3")
      if [ "$run_output" == "hello" ] && [ $run_exit -eq 0 ]; then
        echo -e "${GREEN}✅ PASS${NC} (\"$run_output\")"
        PASSED=$((PASSED + 1))
      else
        echo -e "${RED}❌ FAIL${NC} (output \"$run_output\", exit $run_exit)"
        FAILED=$((FAILED + 1))
        FAILED_TESTS+=("self-hosting (run failed)")
      fi
    fi
  fi

  rm -f "$rebuilt" "$log" "$sh_hello" "$log2" "$log3"
}

# ============================================================
# Main
# ============================================================
echo -e "${BLUE}[Bootstrap]${NC}"
run_bootstrap_tests
echo ""
print_summary
exit $([ $FAILED -eq 0 ])