#!/bin/bash
# test/run_bootstrap_tests.sh — lisp 种子固定点 + 自举测试（7c-1 换锚）。
#
# 旧链（tavm + lib/bootstrap.tabc 自举 build.ta、与提交产物 cmp）随
# codegen/.tabc 世界删除；新自举锚 = 入库的 lib/bootstrap.tabc（driver
# 全闭包，与旧链同名不同物——D3 搬家时定名）。
# 用入库种子把 driver 全闭包重编译一次，然后验证：
#   1. fixed point: 重建产物与入库种子逐字节一致（源码动了没跑
#      `make bootstrap` 更新种子 → 这里红，与旧 gate 语义一致）
#   2. self-hosting: 用重建产物编译 hello.ta，tavm 跑出 "hello"
#   3. G-B（.kl 等价门）: 两条路同产物 —— 重建产物（.ta 路）与
#      .kl 路（`--emit-kl` 写 → `--from-kl` 读回）的 .tabc 逐字节相同
#   4. .kl 路的坏输入要响亮红（不能静默产出半个 .tabc）
#   5. G-C（自足门，§5）: .kl 的稳定性在测试内自足验证，不依赖入库快照
#      （快照退为本地产物，不入 git）：
#      (a) writer 确定性：同一宿主再 emit 一次，与 G-B 现写的 .kl 逐字节相同
#      (b) .kl 路直连种子：G-B 的 from-kl 产物与入库种子逐字节相同
#
# 重建 = driver 全闭包 typecheck（~12s 本地，CI ~3x），前两项检查共享一次
# 重建；G-B 额外两次调用（emit ~7s + from-kl ~1s，后者跳过 typecheck）；
# G-C 额外一次 emit（~7s，writer 确定性检查），from-kl 侧复用 G-B 的产物。
source "$(dirname "$0")/lib.sh"

# Optional SKIP_BOOTSTRAP=1 skips this category (kept for local debugging
# convenience; the rebuild is fast since the typecheck perf fix).
if [ "${SKIP_BOOTSTRAP:-0}" = "1" ]; then
  echo -e "${YELLOW}[Bootstrap] SKIPPED (SKIP_BOOTSTRAP=1)${NC}"
  echo ""
  exit 0
fi

run_bootstrap_tests() {
  local seed="$PROJECT_DIR/lib/bootstrap.tabc"
  local driver_src="$PROJECT_DIR/lib/bootstrap/driver.ta"
  local tavm="${TAVM:-$PROJECT_DIR/tavm}"
  # Single rebuild shared by both checks below
  local rebuilt="/tmp/fp_$$.tabc"
  local log="/tmp/fp_$$.log"
  local t0=$SECONDS
  timeout 300 "$tavm" -q "$seed" "$driver_src" "$rebuilt" "" >"$log" 2>&1
  local build_exit=$?
  local rebuild_secs=$((SECONDS - t0))

  # 1. Fixed point: rebuilt must be bit-identical to the committed seed
  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "bootstrap.tabc fixed point:"
  if [ $build_exit -ne 0 ]; then
    echo -e "${RED}❌ FAIL${NC} (rebuild failed in ${rebuild_secs}s)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("bootstrap.tabc fixed point (rebuild failed)")
  elif cmp -s "$rebuilt" "$seed"; then
    echo -e "${GREEN}✅ PASS${NC} (bit-identical, rebuild ${rebuild_secs}s)"
    PASSED=$((PASSED + 1))
  else
    echo -e "${RED}❌ FAIL${NC} (mismatch, rebuild ${rebuild_secs}s)"
    echo "       seed vs rebuilt differ — run 'make bootstrap' and commit the seed"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("bootstrap.tabc fixed point (mismatch)")
  fi

  # 2. Self-hosting: use the rebuilt driver to compile+run hello.ta
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
    timeout 60 "$tavm" -q "$rebuilt" "$PROJECT_DIR/test/basic/hello.ta" "$sh_hello" "" >"$log2" 2>&1
    if [ $? -ne 0 ]; then
      echo -e "${RED}❌ FAIL${NC} (rebuilt driver can't compile)"
      FAILED=$((FAILED + 1))
      FAILED_TESTS+=("self-hosting (compile failed)")
    else
      timeout 3 "$tavm" -q "$sh_hello" >"$log3" 2>&1
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

  # 3. G-B（.kl 等价门，见 docs/kl-format.md §5）
  #
  # A 路 = 上面那次重建：$rebuilt = compile(lower(driver.ta))（G-D 刚判过它
  # 与入库种子逐字节一致，所以这里比较的是"同一条 .ta 路"的产物）。
  # B 路 = 同一个 lower 产物经 .kl 文本往返：--emit-kl 写文本（.kl 只是多写
  # 一份，编译仍走 .ta 路），--from-kl 把文本读回来编译（不跑 typecheck）。
  # cmp 逐字节 = 与 G-D 同构的判据，不引入新判定语义。
  #
  # 宿主用 $rebuilt（源码刚编出来的驱动）而不是 $seed：接缝参数是**源码**
  # 的能力，入库种子在引入它的那个 PR 里按定义还没有它；种子够不够新由
  # G-D 的 cmp 负责（它红了才有"run make bootstrap"那句提示）。
  local rt_kl="/tmp/kl_gb_$$.kl"
  local rt_bc="/tmp/kl_gb_a_$$.tabc"
  local rt_bc2="/tmp/kl_gb_b_$$.tabc"
  local log4="/tmp/kl_gb4_$$.log"
  local log5="/tmp/kl_gb5_$$.log"
  local bad_kl="/tmp/kl_bad_$$.kl"
  local bad_out="/tmp/kl_bad_$$.tabc"
  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "kl roundtrip (G-B):"
  if [ $build_exit -ne 0 ]; then
    echo -e "${RED}❌ FAIL${NC} (rebuild failed)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("kl roundtrip G-B (rebuild failed)")
  else
    timeout 300 "$tavm" -q "$rebuilt" --emit-kl "$rt_kl" "$driver_src" "$rt_bc" "" >"$log4" 2>&1
    local emit_exit=$?
    if [ $emit_exit -ne 0 ]; then
      echo -e "${RED}❌ FAIL${NC} (--emit-kl failed: $(head -1 "$log4"))"
      FAILED=$((FAILED + 1))
      FAILED_TESTS+=("kl roundtrip G-B (--emit-kl failed)")
    elif [ ! -s "$rt_kl" ]; then
      echo -e "${RED}❌ FAIL${NC} (--emit-kl wrote no .kl)"
      FAILED=$((FAILED + 1))
      FAILED_TESTS+=("kl roundtrip G-B (empty .kl)")
    elif ! cmp -s "$rebuilt" "$rt_bc"; then
      echo -e "${RED}❌ FAIL${NC} (--emit-kl changed the .tabc)"
      FAILED=$((FAILED + 1))
      FAILED_TESTS+=("kl roundtrip G-B (emit perturbed output)")
    else
      timeout 300 "$tavm" -q "$rebuilt" --from-kl "$rt_kl" "$rt_bc2" >"$log5" 2>&1
      if [ $? -ne 0 ]; then
        echo -e "${RED}❌ FAIL${NC} (--from-kl failed: $(head -1 "$log5"))"
        FAILED=$((FAILED + 1))
        FAILED_TESTS+=("kl roundtrip G-B (--from-kl failed)")
      elif cmp -s "$rt_bc" "$rt_bc2"; then
        echo -e "${GREEN}✅ PASS${NC} (bit-identical, .kl $(wc -c <"$rt_kl" | tr -d ' ')B)"
        PASSED=$((PASSED + 1))
      else
        echo -e "${RED}❌ FAIL${NC} (.tabc differs between .ta path and .kl path)"
        echo "       look at od -N 24 (nfns/nconsts): od -N 24 $rt_bc $rt_bc2"
        FAILED=$((FAILED + 1))
        FAILED_TESTS+=("kl roundtrip G-B (.tabc mismatch)")
      fi
    fi
  fi

  # 4. .kl 路的坏输入必须响亮红（§3.3：这条路径没有 typecheck 兜底，所以
  #    "读不进来"必须是 hard error——缺文件与语法错都不能产出半个 .tabc）
  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "kl bad input rejected:"
  timeout 60 "$tavm" -q "$rebuilt" --from-kl "/tmp/kl_missing_$$.kl" "$bad_out" >"$log5" 2>&1
  local miss_exit=$?
  printf '((("\n' > "$bad_kl"
  timeout 60 "$tavm" -q "$rebuilt" --from-kl "$bad_kl" "$bad_out" >>"$log5" 2>&1
  local bad_exit=$?
  if [ $miss_exit -ne 0 ] && [ $bad_exit -ne 0 ] && [ ! -e "$bad_out" ]; then
    echo -e "${GREEN}✅ PASS${NC} (both rc≠0, no .tabc written)"
    PASSED=$((PASSED + 1))
  else
    echo -e "${RED}❌ FAIL${NC} (missing rc=$miss_exit, garbage rc=$bad_exit, out written: $([ -e "$bad_out" ] && echo yes || echo no))"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("kl bad input rejected")
  fi

    # 5. G-C（自足门，见 docs/kl-format.md §5）：.kl 稳定性不靠入库快照
  #    （快照退为本地产物不入 git），在测试内自足验证：
  #      (a) writer 确定性：同一宿主（$rebuilt）再 emit 一次，与 $rt_kl 逐字
  #          节相同 ——「kl 生成是稳定的」的可判形式；
  #      (b) .kl 路直连种子：G-B 的 from-kl 产物 $rt_bc2 与入库种子 $seed
  #          逐字节相同 —— 连上 G-D 的 rebuilt == seed，三条路（.ta / .kl /
  #          种子）闭合成一格，不再需要中间快照当。
    #    宿主用 $rebuilt，与 G-B 同一份信任基础。
  local gc_kl="/tmp/kl_gc_$$.kl"
  local gc_bc="/tmp/kl_gc_$$.tabc"
  local log6="/tmp/kl_gc6_$$.log"
  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "kl emit determinism + seed closure (G-C):"
  if [ $build_exit -ne 0 ]; then
    echo -e "${RED}❌ FAIL${NC} (rebuild failed)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("kl G-C (rebuild failed)")
  else
    timeout 300 "$tavm" -q "$rebuilt" --emit-kl "$gc_kl" "$driver_src" "$gc_bc" "" >"$log6" 2>&1
    if [ $? -ne 0 ]; then
      echo -e "${RED}❌ FAIL${NC} (second --emit-kl failed: $(head -1 "$log6"))"
      FAILED=$((FAILED + 1))
      FAILED_TESTS+=("kl G-C (second emit failed)")
    elif ! cmp -s "$rt_kl" "$gc_kl"; then
      echo -e "${RED}❌ FAIL${NC} (.kl emit not deterministic)"
      echo "       look at diff: $rt_kl $gc_kl"
      FAILED=$((FAILED + 1))
      FAILED_TESTS+=("kl G-C (emit not deterministic)")
    elif ! cmp -s "$rt_bc2" "$seed"; then
      echo -e "${RED}❌ FAIL${NC} (.kl path diverges from the committed seed)"
      echo "       look at od -N 24 (nfns/nconsts): od -N 24 $rt_bc2 $seed"
      FAILED=$((FAILED + 1))
      FAILED_TESTS+=("kl G-C (seed mismatch)")
    else
      echo -e "${GREEN}✅ PASS${NC} (byte-identical ×2, .kl $(wc -c <"$rt_kl" | tr -d ' ')B)"
      PASSED=$((PASSED + 1))
    fi
  fi

  rm -f "$rebuilt" "$log" "$sh_hello" "$log2" "$log3" \
        "$rt_kl" "$rt_bc" "$rt_bc2" "$log4" "$log5" "$bad_kl" "$bad_out" \
        "$gc_kl" "$gc_bc" "$log6"
}

# ============================================================
# Main
# ============================================================
echo -e "${BLUE}[Bootstrap]${NC}"
run_bootstrap_tests
echo ""
print_summary
exit $([ $FAILED -eq 0 ])