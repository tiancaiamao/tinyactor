#!/bin/bash
# test/run_crash_tests.sh — crash visibility tests (issue #28).
#
# Verifies that runtime crashes are observable:
#   a) an actor's div-zero crash prints a CRASH report to stderr (pid,
#      reason symbol, stack frames leaf..root) while the main process
#      survives and the program still exits 0;
#   b) a main-process crash makes the VM exit non-zero;
#   c) a normal program exits 0 with no CRASH on stderr;
#   d) exhausting the process table aborts with a diagnostic instead of
#      corrupting the heap past procs[] (issue #129);
#   e) exhausting an actor's heap arena aborts with a diagnostic instead of
#      returning a half-built value (issue #136);
#   f) a single allocation larger than the arena is rejected the same way;
#   g) a C-function call the host cannot resolve aborts with a diagnostic
#      naming the symbol and the module (issue #203).
#   h) a lazy-dylib module that fails to load (file absent / no
#      vm_load_self / function not provided) aborts at load time with a
#      diagnostic naming the module, the file and the reason (issue #281).
#   i) the compiler's own dylib: lib/sexp.dylib is missing while compiling
#      -> the same load-time diagnostic, not the silent arena exhaustion
#      issue #281 originally reported (regression for the compiler path).
#      The lookup has TWO stages (CWD-relative, then next to the host
#      executable, see src/main.c find_cfunc_autoload/host_module_path), so
#      the scratch dir gets its own copy of tavm: with the repo tavm the
#      second stage would find the repo's lib/sexp and the case could never
#      go red.
#
# Normal category runners (run_test in lib.sh) assert exit 0 and only
# check stdout, so these cases need bespoke assertions (stderr content +
# exit codes). Sourced by test/run_actor_tests.sh after lib.sh and invoked
# via run_crash_tests, so the shared PASSED/FAILED counters stay in one
# process. Can also be run directly: bash test/run_crash_tests.sh

run_crash_tests() {
  local crash_dir="$SCRIPT_DIR/crash"

  # --- run_crash_case: run a .ta via tinyactor run，捕 rc + 日志 ----------
  # 7c-1 翻转：.tabc 构建世界删除——编译+运行统一走 tinyactor run（lisp
  # 双轨：driver 编译半程 → tavm 运行半程）。CRASH 报告（scheduler.c
  # proc_die 共享）与退出码语义不变；lisp 图的帧名/行走靠 main.c 注册的
  # walk_stack/fn_name 钩子（ta.h VM 字段，默认 NULL = vm.c 同享实现）。
  # run_env（可选）仍前缀到运行命令（进程表上限等 env knob）。
  # Sets CRASH_RC / CRASH_EXPECT / CRASH_ERRLOG / CRASH_OUTLOG / CRASH_ID
  # for the assertion helpers below.
  run_crash_case() {
    local id="$1" file="$2" expect_rc="$3" run_env="${4:-}"
    local outlog=$(mktemp "${TMPDIR:-/tmp}/crash_${id}_$$XXXXXX.out")
    local errlog=$(mktemp "${TMPDIR:-/tmp}/crash_${id}_$$XXXXXX.err")

    TOTAL=$((TOTAL + 1))
    printf "  %-50s " "$id:"

    local run_rc=0
    if command -v timeout >/dev/null 2>&1; then
      timeout 60 bash -c "cd '$PROJECT_DIR' && $run_env '$TINYACTOR' run '$file'" >"$outlog" 2>"$errlog"
    else
      bash -c "cd '$PROJECT_DIR' && $run_env '$TINYACTOR' run '$file'" >"$outlog" 2>"$errlog"
    fi
    run_rc=$?

    CRASH_RC=$run_rc
    CRASH_EXPECT=$expect_rc
    CRASH_ERRLOG=$errlog
    CRASH_OUTLOG=$outlog
    CRASH_ID=$id
    return 0
  }

  # --- assertion helpers (consume the CRASH_* globals) ---
  crash_ok() {
    echo -e "${GREEN}✅ PASS${NC}"
    PASSED=$((PASSED + 1))
    rm -f "$CRASH_OUTLOG" "$CRASH_ERRLOG"
  }

    crash_fail() {
    local why="$1"
    echo -e "${RED}❌ FAIL${NC} ($why) — actual rc=$CRASH_RC"
    sed 's/^/      | /' "$CRASH_ERRLOG"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("crash $CRASH_ID ($why)")
    rm -f "$CRASH_OUTLOG" "$CRASH_ERRLOG"
  }

  assert_exit() { [ "$CRASH_RC" -eq "$CRASH_EXPECT" ]; }

  assert_stderr_has() { # needles... — every one must appear in stderr
    local n
    for n in "$@"; do
      grep -qF "$n" "$CRASH_ERRLOG" || return 1
    done
    return 0
  }

  assert_stderr_lacks() { # needles... — none may appear in stderr
    local n
    for n in "$@"; do
      grep -qF "$n" "$CRASH_ERRLOG" && return 1
    done
    return 0
  }

  # ---------------------------------------------------------------
  # (a) actor div-zero crash: report on stderr, main survives, rc=0
  # ---------------------------------------------------------------
      run_crash_case "actor-crash" "$crash_dir/actor-crash.ta" 0
  if assert_exit && assert_stderr_has "CRASH pid" "'divzero" "at crasher"; then
    crash_ok
  else
    crash_fail "expected rc=0, stderr CRASH report with pid + 'divzero + crasher frame"
  fi

  # ---------------------------------------------------------------
  # (a3) actor modulo-by-zero crash: same 'divzero contract via the
  #      % opcode handler (the / cases never execute CASE_OP_MOD).
  # ---------------------------------------------------------------
  run_crash_case "actor-crash-mod" "$crash_dir/actor-crash-mod.ta" 0
  if assert_exit && assert_stderr_has "CRASH pid" "'divzero" "at crasher" \
     && grep -qF "main alive" "$CRASH_OUTLOG"; then
    crash_ok
  else
    crash_fail "expected rc=0, stderr CRASH report with pid + 'divzero + crasher frame, main alive on stdout"
  fi

  # ---------------------------------------------------------------
  # (a2) dynamic non-int % operand: actor dies with 'arithtype, main
  #      survives (issue #158 — previously read the NaN-box payload as
  #      an int: garbage result or spurious 'divzero).
  # ---------------------------------------------------------------
  run_crash_case "mod-type-crash" "$crash_dir/mod-type-crash.ta" 0
  if assert_exit && assert_stderr_has "CRASH pid" "'arithtype" "at crasher" \
     && grep -qF "main alive" "$CRASH_OUTLOG"; then
    crash_ok
  else
    crash_fail "expected rc=0, stderr CRASH report with pid + 'arithtype + crasher frame, main alive on stdout"
  fi

  # ---------------------------------------------------------------
  # (a4) car/cdr of a runtime nil: 'cartype / 'cdrtype, not the silent
  #      nil of issue #236. Both actors die isolated, main survives.
  # ---------------------------------------------------------------
  run_crash_case "car-cdr-nil-crash" "$crash_dir/car-cdr-nil-crash.ta" 0
  if assert_exit && assert_stderr_has "CRASH pid" "'cartype" "at head" "'cdrtype" "at tail" \
     && grep -qF "main alive" "$CRASH_OUTLOG"; then
    crash_ok
  else
    crash_fail "expected rc=0, stderr CRASH reports with 'cartype/'cdrtype + culprit frames, main alive on stdout"
  fi

  # ---------------------------------------------------------------
    # (b) main process crash: host exits non-zero + report
  # ---------------------------------------------------------------
    run_crash_case "main-crash" "$crash_dir/main-crash.ta" 1
  if assert_exit && assert_stderr_has "CRASH pid" "'divzero" "at main"; then
    crash_ok
  else
    crash_fail "expected rc=1, stderr CRASH report with pid + 'divzero + main frame"
  fi

  # ---------------------------------------------------------------
  # (b2) main crashes while an actor is parked in recv: the main-death
  #      path must reap the parked waiter (no hang) and still exit
  #      non-zero with the crash report.
  # ---------------------------------------------------------------
    run_crash_case "main-crash-orphan" "$crash_dir/main-crash-orphan.ta" 1
  if assert_exit && assert_stderr_has "CRASH pid" "'divzero" "at main"; then
    crash_ok
  else
    crash_fail "expected rc=1, stderr CRASH report with pid + 'divzero + main frame"
  fi

  # ---------------------------------------------------------------
  # (b3) call site with too few arguments: the compile-time net left the
  #      backend (arity checks belong to typecheck, issue #376 — and
  #      typecheck only covers the too-many direction), so the VM's CALL
  #      fn_args check is what keeps it loud instead of reading a stale
  #      slot.
  # ---------------------------------------------------------------
  run_crash_case "arity-mismatch" "$crash_dir/arity-mismatch.ta" 1
  if assert_exit && assert_stderr_has "arity mismatch"; then
    crash_ok
  else
    crash_fail "expected rc=1, stderr 'arity mismatch' from the VM CALL check"
  fi


  # ---------------------------------------------------------------
  # (c) normal program: rc=0, no CRASH on stderr
  # ---------------------------------------------------------------
  run_crash_case "normal-exit" "$crash_dir/normal-exit.ta" 0
  if assert_exit && assert_stderr_lacks "CRASH"; then
    crash_ok
  else
    crash_fail "expected rc=0 and no CRASH on stderr"
  fi

  # ---------------------------------------------------------------
  # (d) process-table exhaustion: spawning past the pids table cap must
  #     abort with a diagnostic, not corrupt the heap past procs[].
  #     TA_MAX_PROCS shrinks the table for the RUN only (4th arg) so this
  #     needs 16 spawns rather than a million.
  # ---------------------------------------------------------------
  run_crash_case "pid-space" "$crash_dir/pid-space.ta" 134 "TA_MAX_PROCS=16"
  if assert_exit && assert_stderr_has "process table exhausted"; then
    crash_ok
  else
    crash_fail "expected rc=134 and a 'process table exhausted' diagnostic"
  fi

  # ---------------------------------------------------------------
  # (e) actor heap arena exhaustion: a live set that cannot fit in a tiny
  #     TA_ACTOR_HEAP must abort with a diagnostic, not return a truncated
  #     structure. TA_ACTOR_HEAP shrinks the arena for the RUN only (4th
  #     arg), so this needs 20000 pairs (~320 KiB) rather than 64 MiB.
  # ---------------------------------------------------------------
  run_crash_case "arena-exhausted" "$crash_dir/arena-exhausted.ta" 134 "TA_ACTOR_HEAP=65536"
  if assert_exit && assert_stderr_has "actor heap arena exhausted"; then
    crash_ok
  else
    crash_fail "expected rc=134 and an 'actor heap arena exhausted' diagnostic"
  fi

  # ---------------------------------------------------------------
  # (f) one object bigger than the whole arena (f): same fatal, raised by
  #     the allocation itself rather than by a stack/heap collision.
  # ---------------------------------------------------------------
  run_crash_case "arena-oversized" "$crash_dir/arena-oversized.ta" 134 "TA_ACTOR_HEAP=65536"
  if assert_exit && assert_stderr_has "actor heap arena exhausted" "does not fit"; then
    crash_ok
  else
    crash_fail "expected rc=134 and a 'does not fit' arena diagnostic"
  fi

  # ---------------------------------------------------------------
      # (g) unresolvable C function: the call must abort the VM with a
  #     diagnostic naming the symbol and the module it was looked up in
  #     (issue #203) instead of pushing nil and running on. cov is a lazy
  #     dylib now, so the miss surfaces at load time via the loader's
  #     "provides no" path, naming the dylib file.
  # ---------------------------------------------------------------
    run_crash_case "cfunc-missing" "$crash_dir/cfunc-missing.ta" 1
    if assert_exit && assert_stderr_has "provides no 'cov.nosuchop'" "(lib/cov" \
     && [ ! -s "$CRASH_OUTLOG" ]; then
    crash_ok
  else
    crash_fail "expected rc=1, a 'provides no' dylib-miss diagnostic, and no stdout"
  fi

  # ---------------------------------------------------------------
  # (h) lazy-dylib load failures (issue #281; ABI checks #264 E3): five
  #     sandboxed lib/ states must each abort at LOAD time with a
  #     diagnostic naming the module, the attempted file and the reason —
  #       (h1) library file absent,
  #       (h2) library without the vm_load_self entry point,
  #       (h3) entry point present but no ABI probe (pre-versioning
  #            build, ta.h predating #264 E3),
  #       (h4) loads but does not provide the function (stale build /
  #            misspelled name),
  #       (h5) ABI probe reports a different version than the VM.
  #     dlopen's lib/<mod>.<ext> is cwd-relative, so each case runs
  #     the prebuilt .tabc from a scratch dir with a controlled lib/
  #     — the repo's own dylibs are never touched, and the cases run
  #     on CI whether or not any lib/*.dylib was built there.
  # ---------------------------------------------------------------
      local dylib_ta="$crash_dir/dylib-missing-errors.ta"
  local dylib_bc dylib_out dylib_err dylib_ext dylib_ok=1 dylib_bad=""
  case "$(uname)" in Darwin) dylib_ext=dylib ;; *) dylib_ext=so ;; esac
  # Per-config module infix, exported by the Makefile (plain / _cov / _asan /
  # _tsan). The loader looks up lib/<mod>$(MOD_SUFFIX).<ext>, so stubs must use
  # the same name — tag logic stays in the Makefile, not re-derived here.
  local dylib_mod="nodyslib${MOD_SUFFIX}"
  dylib_bc=$(mktemp "${TMPDIR:-/tmp}/dylib_missing_$$XXXXXX.tabc")
  dylib_out=$(mktemp "${TMPDIR:-/tmp}/dylib_missing_$$XXXXXX.out")
  dylib_err=$(mktemp "${TMPDIR:-/tmp}/dylib_missing_$$XXXXXX.err")

  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "dylib-missing:"

  if bash -c "cd '$PROJECT_DIR' && '$TINYACTOR' build '$dylib_ta' '$dylib_bc'" >"$dylib_out" 2>"$dylib_err"; then
    local dylib_mode scratch needle dylib_rc
    for dylib_mode in missing noload preabi stale abi; do
      scratch=$(mktemp -d "${TMPDIR:-/tmp}/dylib_scratch_$$XXXXXX")
      case "$dylib_mode" in
                missing)
          needle="cannot load C module 'nodyslib'"
          ;;
        noload)
          mkdir -p "$scratch/lib"
          printf 'int placeholder;\n' >"$scratch/stub.c"
          cc -shared -fPIC -o "$scratch/lib/$dylib_mod.$dylib_ext" "$scratch/stub.c" 2>/dev/null \
            || dylib_ok=0
          needle="does not export vm_load_self"
          ;;
        preabi)
          # A module from before ABI versioning: it has the entry point but no
          # ta_abi_version probe. The loader must reject it on the missing
          # probe (h5's sibling), not run it blindly against the new ABI.
          mkdir -p "$scratch/lib"
          printf 'void vm_load_self(void *vm) { (void)vm; }\n' >"$scratch/stub.c"
          cc -shared -fPIC -o "$scratch/lib/$dylib_mod.$dylib_ext" "$scratch/stub.c" 2>/dev/null \
            || dylib_ok=0
          needle="exports no ABI version"
          ;;
        stale)
          # A real module includes ta.h, which is where the ABI probe comes
          # from; including it here keeps this stub on the VM's own ABI so the
          # case still exercises the "entry point ran but function absent"
          # diagnostic rather than tripping an ABI check first.
          mkdir -p "$scratch/lib"
          printf '#include "ta.h"\nvoid vm_load_self(void *vm) { (void)vm; }\n' >"$scratch/stub.c"
          cc -shared -fPIC -I"$PROJECT_DIR" -o "$scratch/lib/$dylib_mod.$dylib_ext" "$scratch/stub.c" 2>/dev/null \
            || dylib_ok=0
          needle="provides no 'nodyslib.thing'"
          ;;
        abi)
          # VM refuses a module built against a different module ABI (issue
          # #264 E3): here the probe reports another version. 0 is never a
          # valid TA_ABI_VERSION (it starts at 1), so it stays wrong even if
          # the constant is later bumped.
          mkdir -p "$scratch/lib"
          printf 'int ta_abi_version(void) { return 0; }\nvoid vm_load_self(void *vm) { (void)vm; }\n' >"$scratch/stub.c"
          cc -shared -fPIC -o "$scratch/lib/$dylib_mod.$dylib_ext" "$scratch/stub.c" 2>/dev/null \
            || dylib_ok=0
          needle="ABI mismatch"
          ;;
      esac
      if command -v timeout >/dev/null 2>&1; then
        timeout 60 bash -c "cd '$scratch' && '$PROJECT_DIR/tavm' -q '$dylib_bc'" >"$dylib_out" 2>"$dylib_err"
      else
        bash -c "cd '$scratch' && '$PROJECT_DIR/tavm' -q '$dylib_bc'" >"$dylib_out" 2>"$dylib_err"
      fi
      dylib_rc=$?
      if [ "$dylib_rc" -ne 1 ] || ! grep -qF "module 'nodyslib'" "$dylib_err" \
         || ! grep -qF "$needle" "$dylib_err" || [ -s "$dylib_out" ]; then
        dylib_ok=0
        dylib_bad="$dylib_bad $dylib_mode(rc=$dylib_rc:$(head -2 "$dylib_err" | tr '\n' '|'))"
      fi
      rm -rf "$scratch"
    done
    if [ "$dylib_ok" -eq 1 ]; then
      echo -e "${GREEN}✅ PASS${NC}"
      PASSED=$((PASSED + 1))
    else
      echo -e "${RED}❌ FAIL${NC} ($dylib_bad)"
      FAILED=$((FAILED + 1))
      FAILED_TESTS+=("crash dylib-missing (load-failure diagnostic: $dylib_bad)")
    fi
  else
    echo -e "${RED}❌ FAIL${NC} (could not build $dylib_ta)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("crash dylib-missing (build of test .tabc failed)")
  fi
  rm -f "$dylib_bc" "$dylib_out" "$dylib_err"

  # ---------------------------------------------------------------
  # (i) the compiler's OWN dylib (issue #281 -> C3): the driver used to read
  #     the lib prelude text with sexp.parse (bytecode.lib_src), so compiling
  #     ANY program needed lib/sexp.dylib. Since the reader moved into the
  #     compiler layer (lib/bootstrap/kl.ta) the compile path needs no C module
  #     at all — this case asserts that inversion, in both directions:
  #       (i1) sexp absent -> a plain program still compiles (rc=0, artifact
  #            written, no sexp anywhere in stderr). Before C3 this aborted
  #            with "cannot load C module 'sexp'"; if the compile path ever
  #            regains a dylib dependency, this is the regression.
  #       (i2) sexp absent + a program that DOES `import sexp` -> load-time
  #            diagnostic still names the module and the attempted file, and
  #            is NOT an arena abort. Missing dylibs must never be tolerated
  #            silently (that was #281's failure mode).
  #     The loader looks up lib/<mod>.<ext> CWD-relative first, then next to
  #     the host executable (src/main.c find_cfunc_autoload ->
  #     host_module_path), so the scratch dir gets BOTH: a lib/ of symlinks
  #     with sexp left out, and its own copy of tavm as the host — otherwise
  #     stage 2 finds the repo's sexp and the case is vacuous. Nothing in the
  #     repo is moved or renamed.
  # ---------------------------------------------------------------
  local c_dir c_ext c_out c_err c_rc c_ok=1 c_bad="" c_base
  case "$(uname)" in Darwin) c_ext=dylib ;; *) c_ext=so ;; esac
  c_base="sexp${MOD_SUFFIX}.$c_ext"
  c_dir=$(mktemp -d "${TMPDIR:-/tmp}/compiler_dylib_$$XXXXXX")
  c_out=$(mktemp "${TMPDIR:-/tmp}/compiler_dylib_$$XXXXXX.out")
  c_err=$(mktemp "${TMPDIR:-/tmp}/compiler_dylib_$$XXXXXX.err")
  mkdir -p "$c_dir/lib"
  for f in "$PROJECT_DIR"/lib/*; do
    [ "$(basename "$f")" = "$c_base" ] && continue
    ln -s "$f" "$c_dir/lib/$(basename "$f")"
    done
  # 宿主可执行文件也要落在 scratch 里（见上面注释：查找的第二级是"宿主旁"）
  cp "$PROJECT_DIR/tavm" "$c_dir/tavm" || c_ok=0
  chmod +x "$c_dir/tavm"
  # E2 契约：编译期 stdlib .ta 从发行版 vendor 树解析（docs/modules.md）——
  # scratch 作为一个"用户项目"，预装发行版 vendor 符号链接（仓库即发行版）。
  # loader 的 dylib 查找仍是 lib/ CWD 相对 + 宿主旁两级，不受此影响：sexp
  # 的 dylib 在这两级都不存在（上面 $c_base 的排除 + tavm 拷贝），所以
  # (i2) 的运行期响亮诊断断言保持有效。
  mkdir -p "$c_dir/vendor/github.com/tiancaiamao"
  ln -s "$PROJECT_DIR" "$c_dir/vendor/github.com/tiancaiamao/tinyactor" || c_ok=0
  printf 'fn main() { print(1) }\n' >"$c_dir/prog.ta"
  printf 'import sexp\nfn main() { print(sexp.parse("(a)")) }\n' >"$c_dir/prog-sexp.ta"

  TOTAL=$((TOTAL + 1))
  printf "  %-50s " "compiler-dylib-missing:"

  # run_compiler_dylib_case <src.ta in $c_dir>
  run_compiler_dylib_case() {
    if command -v timeout >/dev/null 2>&1; then
      timeout 60 bash -c "cd '$c_dir' && ./tavm -q '$PROJECT_DIR/lib/bootstrap.tabc' '$1' out.tabc" >"$c_out" 2>"$c_err"
    else
      bash -c "cd '$c_dir' && ./tavm -q '$PROJECT_DIR/lib/bootstrap.tabc' '$1' out.tabc" >"$c_out" 2>"$c_err"
    fi
    c_rc=$?
  }

  # (i1) sexp absent: the compile path needs nothing from lib/sexp.<ext>
  rm -f "$c_dir/out.tabc"
  run_compiler_dylib_case prog.ta
  if [ "$c_rc" -ne 0 ] || [ ! -s "$c_dir/out.tabc" ] || grep -qi "sexp" "$c_err"; then
    c_ok=0
    c_bad="compile-needs-no-dylib(rc=$c_rc:$(head -2 "$c_err" | tr '\n' '|'))"
  fi

  # (i2) sexp absent + a program that DOES `import sexp`: the compile half is
  #      still dylib-free (that is itself C3: source may *mention* sexp), but
  #      the produced image must fail LOUDLY when run — module + attempted
  #      file + reason, never an arena abort (#281). Compile and run are
  #      separate assertions because the loader resolves C modules at run
  #      time, which is exactly why the compiler stopped needing them.
  rm -f "$c_dir/out.tabc"
  run_compiler_dylib_case prog-sexp.ta
  if [ "$c_rc" -ne 0 ] || [ ! -s "$c_dir/out.tabc" ]; then
    c_ok=0
    c_bad="$c_bad sexp-user-compile(rc=$c_rc:$(head -2 "$c_err" | tr '\n' '|'))"
  fi
  if command -v timeout >/dev/null 2>&1; then
    timeout 60 bash -c "cd '$c_dir' && ./tavm -q out.tabc" >"$c_out" 2>"$c_err"
  else
    bash -c "cd '$c_dir' && ./tavm -q out.tabc" >"$c_out" 2>"$c_err"
  fi
  c_rc=$?
  if [ "$c_rc" -ne 1 ] || [ -s "$c_out" ] \
     || ! grep -qF "cannot load C module 'sexp'" "$c_err" \
     || ! grep -qF "lib/$c_base" "$c_err" \
     || grep -qi arena "$c_err"; then
    c_ok=0
    c_bad="$c_bad sexp-user-run(rc=$c_rc:$(head -2 "$c_err" | tr '\n' '|'))"
  fi

  if [ "$c_ok" -eq 1 ]; then
    echo -e "${GREEN}✅ PASS${NC}"
    PASSED=$((PASSED + 1))
  else
    echo -e "${RED}❌ FAIL${NC} ($c_bad)"
    FAILED=$((FAILED + 1))
    FAILED_TESTS+=("crash compiler-dylib-missing (C3 compile path dylib-free: $c_bad)")
  fi
  rm -rf "$c_dir"
  rm -f "$c_out" "$c_err"
}

# Allow direct execution: bash test/run_crash_tests.sh
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  source "$(dirname "$0")/lib.sh"
  run_crash_tests
  print_summary
  exit $([ $FAILED -eq 0 ])
fi