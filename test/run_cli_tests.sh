#!/usr/bin/env bash
# run_cli_tests.sh — CLI behavior from outside the TinyActor checkout.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TINYACTOR="${TINYACTOR:-$PROJECT_DIR/tinyactor}"
TAVM="${TAVM:-$PROJECT_DIR/tavm}"
TAVM_DRIVER_BC="$PROJECT_DIR/lib/bootstrap.tabc"
WORK="$(mktemp -d)"
# NOVENDOR 必须与 $WORK 平级、不能放进 $WORK：$WORK 自己就是个项目（下面
# init 过），往里塞子目录会让"向上找 vendor/"直接命中父项目。

# 它还要求"任何祖先都没有 vendor/"：`mktemp -d` 尊重 TMPDIR，而 coverage-ta
# 把 TMPDIR 指进仓库（Makefile），仓库根自带 vendor/ —— 那样造出来的"无
# vendor 项目"会合法命中仓库的树、照常编译，负例就退化成假通过。
vendor_free_tmp() {
  local base root dir
  for base in "${TMPDIR:-/tmp}" /tmp /var/tmp; do
    [ -d "$base" ] || continue
    root="$(cd "$base" && pwd -P)"
    dir="$root"
    while [ "$dir" != "/" ] && [ ! -d "$dir/vendor" ]; do
      dir="$(dirname "$dir")"
    done
    [ -d "$dir/vendor" ] || { printf '%s\n' "$root"; return 0; }
  done
  return 1
}

# 同一形状的第二个判据：祖先里不能有 tinyactor.deps。仓库根现在自带一份
# （dogfood：vendor/ 是生成物），TMPDIR 指进仓库时"没有描述文件"的负例
# 会误命中。
deps_free_tmp() {
  local base root dir
  for base in "${TMPDIR:-/tmp}" /tmp /var/tmp; do
    [ -d "$base" ] || continue
    root="$(cd "$base" && pwd -P)"
    dir="$root"
    while [ "$dir" != "/" ] && [ ! -f "$dir/tinyactor.deps" ]; do
      dir="$(dirname "$dir")"
    done
    [ -f "$dir/tinyactor.deps" ] || { printf '%s\n' "$root"; return 0; }
  done
  return 1
}

NOVENDOR_BASE="$(vendor_free_tmp)" || {
  echo "CLI TEST FAIL: no vendor-free temp dir for the no-vendor fixture" >&2
  exit 1
}
NOVENDOR="$(mktemp -d "$NOVENDOR_BASE/tinyactor-novendor.XXXXXX")"
# 报错信息里的路径是编译器 realpath 之后的形态，比对前先归一（/tmp 在 macOS
# 上是 /private/tmp 的符号链接，不归一就永远比不中）。
NOVENDOR="$(cd "$NOVENDOR" && pwd -P)"

trap 'rm -rf "$WORK" "$NOVENDOR"' EXIT

[ -x "$TINYACTOR" ] || { echo "CLI TEST FAIL: tinyactor not executable: $TINYACTOR" >&2; exit 1; }
[ -x "$TAVM" ] || { echo "CLI TEST FAIL: tavm not executable: $TAVM" >&2; exit 1; }
[ -s "$TAVM_DRIVER_BC" ] || { echo "CLI TEST FAIL: lisp backend driver missing: $TAVM_DRIVER_BC" >&2; exit 1; }

# 模块解析的根 = 入口源文件向上第一个含 vendor/ 的祖先，候选恒一条
# （docs/modules.md §2/§3）。$WORK 就是一个"用户项目"，用
# `tinyactor init` 给它一条 vendor/<发行树名> -> 安装树的链接 —— 这既是下面
# 所有 fixture 里 import 标准库的前提，也是 init 子命令的冒烟测试。
(cd "$WORK" && "$TINYACTOR" init >/dev/null)

# lisp 编译半程：与 tinyactor run 的 run_tavm 同参布局（driver 编 .tabc）。
# 测试直接喂 driver 是为了在 .tabc 之上测 VM 级 flag（--profile /
# TA_DUMP_INTERNS）——tinyactor run 不透传它们。
compile_lisp() {
    local src="$1" out="$2"
    local cache="${TA_SIG_CACHE_DIR:-$PROJECT_DIR/.build/modules}"
    (cd "$PROJECT_DIR" && env -u TA_ACTOR_HEAP "$TAVM" -q "$TAVM_DRIVER_BC" "$src" "$out" "$cache")
}

cat > "$WORK/fs_cli.ta" <<'EOF'
import "tinyactor:lib/fs"
fn main() {
  if fs.mkdir_p("cli-test-output/nested") == 1 {
    print("CLI RUN PASS")
  } else {
    print("CLI RUN FAIL")
  }
}
EOF

(cd "$WORK" && "$TINYACTOR" run fs_cli.ta | grep -qx 'CLI RUN PASS')
[ -d "$WORK/cli-test-output/nested" ] || { echo "CLI TEST FAIL: run did not create directory" >&2; exit 1; }
echo "ok run from external CWD"

cat > "$WORK/fmt.ta" <<'EOF'
fn main(){print("fmt")}
EOF
(cd "$WORK" && "$TINYACTOR" fmt fmt.ta)
(cd "$WORK" && "$TINYACTOR" fmt --check fmt.ta)
echo "ok fmt from external CWD"

if (cd "$WORK" && "$TINYACTOR" run missing.ta) >"$WORK/missing.out" 2>&1; then
  echo "CLI TEST FAIL: missing source unexpectedly succeeded" >&2
  exit 1
fi
grep -Fqx "error: TinyActor source file not found: $WORK/missing.ta" "$WORK/missing.out"
echo "ok missing source diagnostic"

if (cd "$WORK" && "$TINYACTOR" fmt --check missing.ta) >"$WORK/missing-fmt.out" 2>&1; then
  echo "CLI TEST FAIL: missing fmt source unexpectedly succeeded" >&2
  exit 1
fi
grep -Fqx "error: TinyActor source file not found: $WORK/missing.ta" "$WORK/missing-fmt.out"
echo "ok missing fmt source diagnostic"

# Sampling profiler smoke test: --profile must produce speedscope json +
# folded stacks even for a trivial program (exercises src/prof.c sampling,
# collection and both writers; the plain suite never enables the flag).
# The generated entry points matter: sampling keys are whole stacks and
# the walk cap is 64 frames, so 600 shallow f_i -> work(40) stacks push
# the profiler hash table (init cap 1024, rehash at 75% load) past 716
# entries and force the rehash path — a single-function program can
# never reach it.
{
    echo 'fn work(n) { if n == 0 { 0 } else { work(n - 1) + 1 } }'
    i=1
    while [ $i -le 600 ]; do
        printf "fn f$i() {"
        j=0
        while [ $j -lt 15 ]; do
            printf " work(40) +"
            j=$((j + 1))
        done
        echo " $i }"
        i=$((i + 1))
    done
    echo 'fn main() {'
    echo '  work(20000)'
    i=1
    while [ $i -le 600 ]; do
        echo "  let _ = f$i()"
        i=$((i + 1))
    done
    echo '  print("PROF RUN PASS")'
    echo '}'
} > "$WORK/prof.ta"
compile_lisp "$WORK/prof.ta" "$WORK/prof.tabc"
[ -s "$WORK/prof.tabc" ] || { echo "CLI TEST FAIL: lisp compile produced no bytecode" >&2; exit 1; }
echo "ok lisp backend compile"

# .kl 接缝的用户面入口（docs/kl-format.md §1）：build --emit-kl 只写 .kl 文本
# （gcc -S 语义，无字节码产物），与 driver 层直接写的逐字节相同；build xxx.kl
# 按扩展名分发走 from-kl 路（不读源码、不跑 typecheck），重建出的镜像与直接
# build 逐字节相同。
"$TINYACTOR" build --emit-kl "$WORK/prof.ta" "$WORK/emit.kl"
[ -s "$WORK/emit.kl" ] || { echo "CLI TEST FAIL: build --emit-kl wrote no .kl" >&2; exit 1; }
(cd "$PROJECT_DIR" && env -u TA_ACTOR_HEAP "$TAVM" -q "$TAVM_DRIVER_BC" \
    --emit-kl "$WORK/driver.kl" "$WORK/prof.ta" "$WORK/driver.tabc" "")
cmp "$WORK/emit.kl" "$WORK/driver.kl" || {
  echo "CLI TEST FAIL: build --emit-kl text differs from the driver seam" >&2
  exit 1
}
"$TINYACTOR" build "$WORK/emit.kl" "$WORK/fromkl.tabc"
cmp "$WORK/fromkl.tabc" "$WORK/prof.tabc" || {
  echo "CLI TEST FAIL: build <in.kl> image differs from a direct build" >&2
  exit 1
}
"$TAVM" -q "$WORK/fromkl.tabc" | grep -qx 'PROF RUN PASS'
echo "ok .kl emit / from-kl seam (byte-identical to the driver seam)"

if "$TINYACTOR" build "$WORK/missing.kl" "$WORK/missing.tabc" >"$WORK/kl-missing.out" 2>&1; then
  echo "CLI TEST FAIL: build on a missing .kl unexpectedly succeeded" >&2
  exit 1
fi
grep -Fqx "error: .kl file not found: $WORK/missing.kl" "$WORK/kl-missing.out"
echo "ok missing .kl diagnostic"

# 扩展名分发 + 默认输出名：build xxx.ta → 旁边 xxx.tabc；build --emit-kl
# xxx.ta → 旁边 xxx.kl（无字节码）；build xxx.kl → 旁边 xxx.tabc；run xxx.kl
# 按扩展名分发。
cat > "$WORK/defname.ta" <<'EOF'
fn main() { print("DEFNAME PASS") }
EOF
rm -f "$WORK/defname.kl" "$WORK/defname.tabc"
(cd "$WORK" && "$TINYACTOR" build defname.ta)
[ -s "$WORK/defname.tabc" ] || { echo "CLI TEST FAIL: build defname.ta wrote no default defname.tabc" >&2; exit 1; }
"$TAVM" -q "$WORK/defname.tabc" | grep -qx 'DEFNAME PASS'
rm -f "$WORK/defname.tabc"
(cd "$WORK" && "$TINYACTOR" build --emit-kl defname.ta)
[ -s "$WORK/defname.kl" ] || { echo "CLI TEST FAIL: build --emit-kl defname.ta wrote no default defname.kl" >&2; exit 1; }
[ ! -e "$WORK/defname.tabc" ] || { echo "CLI TEST FAIL: build --emit-kl wrote a bytecode image" >&2; exit 1; }
(cd "$WORK" && "$TINYACTOR" build defname.kl)
[ -s "$WORK/defname.tabc" ] || { echo "CLI TEST FAIL: build defname.kl wrote no default defname.tabc" >&2; exit 1; }
(cd "$WORK" && "$TINYACTOR" run defname.kl | grep -qx 'DEFNAME PASS')
echo "ok extension dispatch + default output names"

# ---------- .tabc 直跑（issue #367） ----------
# run 认自家产物：.tabc 交给 tavm 原样执行（等价 `tavm -q <img> args…`），
# argv / rc 透传。**不编译**这条给三条独立证据，不靠"看起来快"：
#   ① 镜像与源分叉（源改成另一个输出、镜像不动）时行为跟镜像走；
#   ② 跑完镜像逐字节不变（没有原地重写它）；
#   ③ 编译半程的 sig cache 目录压根不被创建（连编译半程都没进）。
cat > "$WORK/image.ta" <<'EOF'
import "tinyactor:lib/os"
fn show(l) {
  if null?(l) { 0 } else {
    print(car(l))
    show(cdr(l))
  }
}
fn main() {
  show(os.args())
  print("IMAGE PASS")
}
EOF
"$TINYACTOR" build "$WORK/image.ta" "$WORK/image.tabc"

# argv 透传：目标参数一个不落地进程序 —— 含 CLI/宿主的 flag 拼法
# (--trace / -q)，它们出现在镜像路径**之后**，只属于目标程序。
(cd "$WORK" && "$TINYACTOR" run image.tabc --name=alice --trace 5 -q tail) >"$WORK/image-args.out"
for want in "IMAGE PASS" "$WORK/image.tabc" "--name=alice" "--trace" "5" "-q" "tail"; do
  grep -Fqx -- "$want" "$WORK/image-args.out" || {
    echo "CLI TEST FAIL: run .tabc lost an argv token: $want" >&2
    cat "$WORK/image-args.out" >&2
    exit 1
  }
done
echo "ok run .tabc argv passthrough"

# rc 透传：程序自己的退出码原样成为 CLI 的退出码（exec，无中间层）。
cat > "$WORK/image-rc.ta" <<'EOF'
import "tinyactor:lib/os"
fn main() { os.exit(7) }
EOF
"$TINYACTOR" build "$WORK/image-rc.ta" "$WORK/image-rc.tabc"
image_rc=0
(cd "$WORK" && "$TINYACTOR" run image-rc.tabc) || image_rc=$?
[ "$image_rc" -eq 7 ] || { echo "CLI TEST FAIL: run .tabc rc=$image_rc (want 7)" >&2; exit 1; }
echo "ok run .tabc rc passthrough"

# 不编译（① ② ③）：源被改写成另一个输出后，跑镜像仍拿镜像里的行为
# （若 CLI 悄悄回落去编源，这里必然打印 FROM SOURCE）。
cp "$WORK/image.tabc" "$WORK/image.tabc.orig"
cat > "$WORK/image.ta" <<'EOF'
fn main() { print("FROM SOURCE (must not appear)") }
EOF
(cd "$WORK" && "$TINYACTOR" run image.tabc) | grep -qx 'IMAGE PASS' || {
  echo "CLI TEST FAIL: run .tabc recompiled the diverged source" >&2
  exit 1
}
cmp "$WORK/image.tabc" "$WORK/image.tabc.orig" || {
  echo "CLI TEST FAIL: run .tabc rewrote the image" >&2
  exit 1
}
(cd "$WORK" && TA_SIG_CACHE_DIR="$WORK/no-sig-cache" "$TINYACTOR" run image.tabc) | grep -qx 'IMAGE PASS' || {
  echo "CLI TEST FAIL: run .tabc failed under a fresh sig cache dir" >&2
  exit 1
}
[ ! -e "$WORK/no-sig-cache" ] || {
  echo "CLI TEST FAIL: run .tabc created the compile sig cache: $WORK/no-sig-cache" >&2
  exit 1
}
# 对照：同一份源走 .ta 路照旧编译（既有路径不回归）。
(cd "$WORK" && "$TINYACTOR" run image.ta) | grep -qx 'FROM SOURCE (must not appear)' || {
  echo "CLI TEST FAIL: run .ta stopped compiling the source" >&2
  exit 1
}
echo "ok run .tabc does not compile (source divergence + untouched image + no sig cache)"

# 失败要响，且**不回落**去编译源文件：缺镜像 / 坏镜像都是非 0 + 明确诊断，
# 同名 .ta 就在旁边也不编它（回落会把"跑镜像"静默变成"编源跑"）。
cat > "$WORK/nofallback.ta" <<'EOF'
fn main() { print("FALLBACK BAD") }
EOF
if (cd "$WORK" && "$TINYACTOR" run nofallback.tabc) >"$WORK/image-missing.out" 2>&1; then
  echo "CLI TEST FAIL: missing image unexpectedly succeeded" >&2
  exit 1
fi
grep -Fqx "error: bytecode image not found: $WORK/nofallback.tabc" "$WORK/image-missing.out" || {
  echo "CLI TEST FAIL: missing image not diagnosed" >&2
  cat "$WORK/image-missing.out" >&2
  exit 1
}
head -c 20 "$WORK/image.tabc.orig" > "$WORK/nofallback.tabc"
if (cd "$WORK" && "$TINYACTOR" run nofallback.tabc) >"$WORK/image-broken.out" 2>&1; then
  echo "CLI TEST FAIL: truncated image unexpectedly succeeded" >&2
  exit 1
fi
if grep -q 'FALLBACK BAD' "$WORK/image-missing.out" "$WORK/image-broken.out"; then
  echo "CLI TEST FAIL: run fell back to compiling the source" >&2
  exit 1
fi
echo "ok run .tabc missing/broken image diagnosed, no fallback to compiling"

# build 不认 .tabc（镜像已经是它的产物、不是输入）：认了扩展名却不报错
# 会让这条命令静默成功（什么都不做）。
if (cd "$WORK" && "$TINYACTOR" build image.tabc) >"$WORK/build-tabc.out" 2>&1; then
  echo "CLI TEST FAIL: build <img>.tabc unexpectedly succeeded" >&2
  exit 1
fi
grep -Fqx "error: build takes a .ta or .kl source, got: $WORK/image.tabc" "$WORK/build-tabc.out" || {
  echo "CLI TEST FAIL: build <img>.tabc not diagnosed" >&2
  cat "$WORK/build-tabc.out" >&2
  exit 1
}
# usage 里 run 接受的扩展名同步到 .tabc。
"$TINYACTOR" help >"$WORK/usage.out" 2>&1 || true
grep -qF '.ta, .kl or .tabc' "$WORK/usage.out" || {
  echo "CLI TEST FAIL: usage does not document .tabc for run" >&2
  exit 1
}
echo "ok build rejects .tabc + usage documents it"



"$TAVM" -q --profile="$WORK/prof-out" "$WORK/prof.tabc" | grep -qx 'PROF RUN PASS'
[ -s "$WORK/prof-out.json" ] || { echo "CLI TEST FAIL: --profile produced no speedscope json" >&2; exit 1; }
[ -s "$WORK/prof-out.folded" ] || { echo "CLI TEST FAIL: --profile produced no folded stacks" >&2; exit 1; }
grep -qF "work" "$WORK/prof-out.folded" || {
  echo "CLI TEST FAIL: folded stacks contain no function names" >&2
  exit 1
}
echo "ok --profile smoke"

# A malformed --profile value (non-empty, not "=base") must be rejected,
# not silently ignored (this error branch is otherwise never hit).
if "$TAVM" --profilebad "$WORK/prof.tabc" >"$WORK/prof-bad.out" 2>&1; then
  echo "CLI TEST FAIL: malformed --profile unexpectedly succeeded"
  exit 1
fi
grep -q "error: unknown option: --profilebad" "$WORK/prof-bad.out" || {
  echo "CLI TEST FAIL: malformed --profile not diagnosed"
  exit 1
}
echo "ok malformed --profile rejected"

# Bare --profile (no =base) defaults the output base to "profile" in the
# current working directory.
(cd "$WORK" && "$TAVM" -q --profile "$WORK/prof.tabc" | grep -qx 'PROF RUN PASS')
[ -s "$WORK/profile.json" ] || {
  echo "CLI TEST FAIL: bare --profile did not write profile.json in CWD"
  exit 1
}
echo "ok bare --profile default base"

# Invoking tavm with no image must print usage and exit non-zero (the
# `if (!path)` branch is otherwise never hit).
if "$TAVM" >"$WORK/noimage.out" 2>&1; then
  echo "CLI TEST FAIL: missing image unexpectedly succeeded"
  exit 1
fi
grep -q "usage: " "$WORK/noimage.out" || {
  echo "CLI TEST FAIL: missing image did not print usage"
  exit 1
}
echo "ok missing image usage"

# TA_ACTOR_HEAP: a valid value must be honoured and a normal run must
# still pass (the env-arena-size parse path is otherwise never hit).
TA_ACTOR_HEAP=33554432 "$TAVM" -q "$WORK/prof.tabc" | grep -qx 'PROF RUN PASS'
echo "ok TA_ACTOR_HEAP honoured"

# Non-quiet run: without -q the host prints main's exit value after the
# program's own stdout (the print_val(exit_val) tail is otherwise always
# skipped by -q).
"$TAVM" "$WORK/prof.tabc" > "$WORK/prof-loud.out"
grep -qx 'PROF RUN PASS' "$WORK/prof-loud.out" || {
  echo "CLI TEST FAIL: non-quiet run lost program stdout"
  exit 1
}
grep -qx 'nil' <(tail -n 1 "$WORK/prof-loud.out") || {
  echo "CLI TEST FAIL: non-quiet run did not print the exit value"
  exit 1
}
echo "ok non-quiet exit value"

# Intern-table dump diagnostic: TA_DUMP_INTERNS=<path> must write one
# "idx name" line per interned symbol after a normal run (PR #104).
TA_DUMP_INTERNS="$WORK/interns.txt" "$TAVM" -q "$WORK/prof.tabc" > /dev/null
[ -s "$WORK/interns.txt" ] || { echo "CLI TEST FAIL: TA_DUMP_INTERNS produced no dump" >&2; exit 1; }
grep -qE '^0 [a-z]' "$WORK/interns.txt" || {
  echo "CLI TEST FAIL: intern dump missing 'idx name' lines" >&2
  exit 1
}
echo "ok TA_DUMP_INTERNS"

# Loader hardening: empty and truncated images must be rejected with a
# non-zero exit, never executed (tavm parse_unit file/hardening paths).
: > "$WORK/empty.tabc"
if "$TAVM" "$WORK/empty.tabc" > "$WORK/empty.out" 2>&1; then
  echo "CLI TEST FAIL: empty .tabc unexpectedly loaded" >&2
  exit 1
fi
head -c 20 "$WORK/prof.tabc" > "$WORK/trunc.tabc"
if "$TAVM" "$WORK/trunc.tabc" > "$WORK/trunc.out" 2>&1; then
  echo "CLI TEST FAIL: truncated .tabc unexpectedly loaded" >&2
  exit 1
fi
echo "ok empty/truncated .tabc rejected"

# TA_MAX_PROCS: invalid values are ignored (never truncate the table);
# the program still runs normally. api.c 的进程表 cap 同时约束 tavm。
if TA_MAX_PROCS=notanumber "$TINYACTOR" run "$WORK/prof.ta" | grep -qx 'PROF RUN PASS' &&
   TA_MAX_PROCS=1 "$TINYACTOR" run "$WORK/prof.ta" | grep -qx 'PROF RUN PASS'; then
  echo "ok TA_MAX_PROCS invalid values ignored"
else
  echo "CLI TEST FAIL: TA_MAX_PROCS=invalid broke a normal run" >&2
  exit 1
fi

# Wide-module arena regression (#308): a def-heavy module must compile under
# the default 64MB actor arena. flatten_fns was non-tail, so each def pinned a
# growing const-pool copy on its stack frame → O(N²) live pairs; ~1000 defs
# already exhausted the arena (heap 65MB, abort). The count only needs to
# cross the old cliff, not scale the benchmark.
: > "$WORK/wide.ta"
for k in $(seq 1 1000); do
  printf 'pub fn h%d() {\n  "w%d"\n}\n\n' "$k" "$k" >> "$WORK/wide.ta"
done
{
  echo 'fn main() {'
  for k in $(seq 1 1000); do echo "  h$k()"; done
  echo '}'
} >> "$WORK/wide.ta"
if (cd "$WORK" && "$TINYACTOR" build wide.ta wide.tabc); then
  echo "ok wide module (1000 defs) compiles under default arena (#308)"
else
  echo "CLI TEST FAIL: wide module exhausted the default arena (#308)" >&2
  exit 1
fi
# SIGINT graceful stop (#358): a long-running program (main blocks in recv)
# must exit rc=0 on SIGINT and still print its exit value — not die silently
# via the default disposition (rc=130, no output). The signal is delivered to
# the process directly, emulating Ctrl+C.
cat > "$WORK/sigint.ta" <<'EOF'
fn main() {
  print("SIGINT READY")
  let x = recv()
  print(x)
}
EOF
compile_lisp "$WORK/sigint.ta" "$WORK/sigint.tabc"
"$TAVM" "$WORK/sigint.tabc" > "$WORK/sigint.out" 2>&1 &
sig_pid=$!
# Watchdog: kill -9 after 10s so a VM that ignores SIGINT fails (rc=137)
# instead of hanging the suite.
( sleep 10; kill -9 "$sig_pid" 2>/dev/null ) &
watchdog=$!
sleep 1
kill -INT "$sig_pid"
sig_rc=0
wait "$sig_pid" || sig_rc=$?
kill "$watchdog" 2>/dev/null || true
wait "$watchdog" 2>/dev/null || true
[ "$sig_rc" -eq 0 ] || {
  echo "CLI TEST FAIL: SIGINT stop rc=$sig_rc (want 0)" >&2
  cat "$WORK/sigint.out" >&2
  exit 1
}
grep -q 'SIGINT READY' "$WORK/sigint.out" || {
  echo "CLI TEST FAIL: SIGINT run lost program output" >&2
  exit 1
}
echo "ok SIGINT graceful stop"
# ---------- 消费项目（issue #368 重造） ----------
# fixture test/consumer 是个普通用户项目：入 git 的只有 tinyactor.deps，
# vendor/ 由 `tinyactor vendor` 现造（这里先跑一遍 —— 它同时是「工具在真实
# 项目上工作」的那条用例）。根 = 它自己（入口文件向上第一个含 vendor/ 的
# 祖先），vendor/ 下有三棵树 —— 项目自己的 example.org/consumer（自指）、
# 第三方 example.org/hello 的项目副本（源码在 deps/hello/）、工具链标准库
# tinyactor -> 仓库。单根 + 一条候选意味着安装树那份 hello 不参与解析，
# 命中的必然是项目里的。
CONSUMER="$PROJECT_DIR/test/consumer"
(cd "$CONSUMER" && rm -rf vendor && "$TINYACTOR" vendor) || {
  echo "CLI TEST FAIL: tinyactor vendor failed in the consumer fixture" >&2
  exit 1
}
for link in "vendor/example.org/consumer" "vendor/example.org/hello" "vendor/tinyactor"; do
  [ -e "$CONSUMER/$link" ] || {
    echo "CLI TEST FAIL: consumer fixture link missing: $CONSUMER/$link" >&2
    exit 1
  }
done
consumer_check() {
  printf '%s\n' "$consumer_run" | grep -qx "$1" || {
    echo "CLI TEST FAIL: consumer project: $2 (missing line: $1)" >&2
    printf '%s\n' "$consumer_run" >&2
    exit 1
  }
}
consumer_run="$(cd "$CONSUMER" && "$TINYACTOR" run app/main.ta)"
consumer_check 'consumer: project-local module' 'self-referential tree module not resolved'
consumer_check 'consumerlib: from the project vendor tree' "project-only module ('example.org/consumer:lib/consumerlib') not resolved"
consumer_check 'json: from the project vendor tree' "project's own tree did not answer 'example.org/consumer:lib/json'"
consumer_check "hello from the consumer project's vendor tree" 'project copy of example.org/hello did not win (single root)'
consumer_check 'CONSUMER VENDOR OK' 'run never reached the pass marker'

# 根只由入口文件位置决定，与 shell cwd 无关：同一个入口文件从 app/ 里跑必须
# 完全一样（旧契约把根绑在 $PWD 上，实测这条会 module not found —— §2）。
(cd "$CONSUMER/app" && "$TINYACTOR" run main.ta) >"$WORK/consumer-subdir.out" 2>&1 || {
  echo "CLI TEST FAIL: consumer run from a subdirectory failed" >&2
  cat "$WORK/consumer-subdir.out" >&2
  exit 1
}
printf '%s\n' "$consumer_run" >"$WORK/consumer-root.out"
cmp "$WORK/consumer-root.out" "$WORK/consumer-subdir.out" || {
  echo "CLI TEST FAIL: consumer output depends on the caller's cwd" >&2
  exit 1
}

# build 是独立出口（run = build + tavm）：同一基准，产物直接可跑。
(cd "$CONSUMER" && "$TINYACTOR" build app/main.ta "$WORK/consumer.tabc")
[ -s "$WORK/consumer.tabc" ] || { echo "CLI TEST FAIL: consumer build wrote no image" >&2; exit 1; }
"$TAVM" -q "$WORK/consumer.tabc" | grep -qx 'CONSUMER VENDOR OK' || {
  echo "CLI TEST FAIL: consumer-built image lost the project modules" >&2
  exit 1
}
echo "ok consumer project (single root, cwd-independent, build == run)"

# 没有 vendor/ 的目录**不能**编译：不回退安装树、不看 cwd，报三行 + 提示
# init（这是唯一的硬前置：modules.md §2 第 5 条，报告文案见 §8）。
cat > "$NOVENDOR/nostdlib.ta" <<'EOF'
import "tinyactor:lib/str"
fn main() { print(str.concat("NO ", "VENDOR")) }
EOF
if (cd "$NOVENDOR" && "$TINYACTOR" run nostdlib.ta) >"$NOVENDOR/run.out" 2>&1; then
  echo "CLI TEST FAIL: vendor-less project unexpectedly compiled" >&2
  exit 1
fi
grep -Fqx "error: no vendor/ directory above $NOVENDOR/nostdlib.ta" "$NOVENDOR/run.out" || {
  echo "CLI TEST FAIL: vendor-less project not diagnosed" >&2
  cat "$NOVENDOR/run.out" >&2
  exit 1
}
grep -Fq 'run `tinyactor init` in your project to create one' "$NOVENDOR/run.out" || {
  echo "CLI TEST FAIL: vendor-less diagnostic lost the init hint" >&2
  cat "$NOVENDOR/run.out" >&2
  exit 1
}
echo "ok vendor-less project rejected with an init hint"

# ... 而 `tinyactor init` 正是那条恢复路径：建一条树链接，同一条命令就编得过。
(cd "$NOVENDOR" && "$TINYACTOR" init)
(cd "$NOVENDOR" && "$TINYACTOR" run nostdlib.ta) | grep -qx 'NO VENDOR' || {
  echo "CLI TEST FAIL: tinyactor init did not make the stdlib importable" >&2
  exit 1
}
# 重复 init 是幂等的（第二次什么都不该改）。
(cd "$NOVENDOR" && "$TINYACTOR" init) | grep -q '已是最新' || {
  echo "CLI TEST FAIL: tinyactor init is not idempotent" >&2
  exit 1
}
echo "ok tinyactor init scaffolds tinyactor.deps + vendor/ (idempotent)"

# ---------- tinyactor vendor：tinyactor.deps → vendor/ ----------
# 入 git 的只有描述文件，vendor/ 是生成物（docs/modules.md §10）。这里验：
#   (a) 两列到底 —— 自指树与本地树各一条链接，且解析真能跑通
#   (b) 幂等：第二次跑不动任何链接的 mtime（下游增量判定把 vendor/ 当输入）
#   (c) 描述文件写错就报错，不静默生成半套
VDEPS="$WORK/depsproj"
mkdir -p "$VDEPS/lib"
cat > "$VDEPS/lib/greet.ta" <<'EOF'
pub fn hi() -> String { "hi from mylib" }
EOF
cat > "$VDEPS/main.ta" <<'EOF'
import "mylib:lib/greet" as g
import "tinyactor:lib/str" as s
fn main() { print(s.concat(g.hi(), " + str")) }
EOF
cat > "$VDEPS/tinyactor.deps" <<EOF
# 注释行与空行都要被跳过

mylib	.
tinyactor	$PROJECT_DIR
EOF
(cd "$VDEPS" && "$TINYACTOR" vendor) | grep -q '2 棵树' || {
  echo "CLI TEST FAIL: tinyactor vendor did not materialise the deps file" >&2
  exit 1
}
[ "$(readlink "$VDEPS/vendor/mylib")" = ".." ] || {
  echo "CLI TEST FAIL: deps \`.\` did not become the project self-link" >&2
  exit 1
}
(cd "$VDEPS" && "$TINYACTOR" run main.ta) | grep -qx 'hi from mylib + str' || {
  echo "CLI TEST FAIL: deps-generated vendor/ does not resolve imports" >&2
  exit 1
}
# 幂等：链接的 mtime 不变（ln -sfn 无条件跑会改，下游据此判定镜像过期）
stamp_before=$(stat -f '%m%c' "$VDEPS/vendor/tinyactor" 2>/dev/null || stat -c '%Y%Z' "$VDEPS/vendor/tinyactor")
(cd "$VDEPS" && "$TINYACTOR" vendor) | grep -q '已是最新' || {
  echo "CLI TEST FAIL: second tinyactor vendor was not a no-op" >&2
  exit 1
}
stamp_after=$(stat -f '%m%c' "$VDEPS/vendor/tinyactor" 2>/dev/null || stat -c '%Y%Z' "$VDEPS/vendor/tinyactor")
[ "$stamp_before" = "$stamp_after" ] || {
  echo "CLI TEST FAIL: tinyactor vendor rewrote an already-correct link" >&2
  exit 1
}
echo "ok tinyactor vendor generates vendor/ from tinyactor.deps (idempotent)"

# 描述文件的三种写法错误都要拒绝，且不许留下半套 vendor/
vdep_fail() {
  local body="$1" want="$2"
  printf '%s\n' "$body" > "$VDEPS/tinyactor.deps"
  rm -rf "$VDEPS/vendor"
  if (cd "$VDEPS" && "$TINYACTOR" vendor) >"$VDEPS/vendor.out" 2>&1; then
    echo "CLI TEST FAIL: tinyactor.deps accepted bad line: $body" >&2
    exit 1
  fi
  grep -Fq "$want" "$VDEPS/vendor.out" || {
    echo "CLI TEST FAIL: wrong diagnostic for tinyactor.deps line: $body" >&2
    cat "$VDEPS/vendor.out" >&2
    exit 1
  }
}
vdep_fail 'mylib	./nope' "来源不是目录"
vdep_fail 'mylib' "缺来源"
vdep_fail 'mylib	.  v1.0.0' "版本固定还没实现"
vdep_fail '../evil	.' "非法树名"
vdep_fail 'mylib	.
mylib	.' "树名重复"
# 没有 tinyactor.deps 的目录：命令拒绝，并指出要写什么
NODEPS_BASE="$(deps_free_tmp)" || {
  echo "CLI TEST FAIL: no deps-free temp dir for the no-deps fixture" >&2
  exit 1
}
NODEPS="$(mktemp -d "$NODEPS_BASE/tinyactor-nodeps.XXXXXX")"
trap 'rm -rf "$WORK" "$NOVENDOR" "$NODEPS"' EXIT
if (cd "$NODEPS" && "$TINYACTOR" vendor) >"$VDEPS/nodeps.out" 2>&1; then
  echo "CLI TEST FAIL: tinyactor vendor without a deps file unexpectedly succeeded" >&2
  exit 1
fi
grep -Fq 'no tinyactor.deps above' "$VDEPS/nodeps.out" || {
  echo "CLI TEST FAIL: missing-deps diagnostic is unclear" >&2
  cat "$VDEPS/nodeps.out" >&2
  exit 1
}
echo "ok tinyactor vendor rejects malformed tinyactor.deps"

# ---------- TA 覆盖率工具链（--cov / covmap / dump / html） ----------
# Coverage instrumentation: `build --cov` rewrites fn entries AND every
# statement / match-arm body to cov.hit(k) and emits a <out>.covmap side
# table ("k file:line name" rows, line 0 = fn entry). Running the
# instrumented image and dumping must reproduce the covmap ids with the
# right counts (join-ability is the whole contract), including missed
# statements reporting count 0.
cat > "$WORK/covprog.ta" <<'EOF'
import "tinyactor:lib/cov"
fn leaf(x) {
  x + 1
}

fn grade(x) {
  let y = x + 1
  match y {
    1 -> 10
    _ -> 20
  }
}

fn main() {
  let a = leaf(1)
  let g = grade(0)
  cov.hit(99)
  cov.dump("/tmp/tinyactor-cov-cli-dump.txt")
  print(a + g)
}

fn noop() { }
EOF
(cd "$WORK" && "$TINYACTOR" build --cov covprog.ta covprog.tabc)
[ -s "$WORK/covprog.tabc" ] || { echo "CLI TEST FAIL: --cov build produced no bytecode" >&2; exit 1; }
grep -qE '^0 .*covprog\.ta:0 leaf$' "$WORK/covprog.tabc.covmap" || {
  echo "CLI TEST FAIL: covmap missing '0 covprog.ta:0 leaf'" >&2
  exit 1
}
grep -qE '^2 .*covprog\.ta:0 grade$' "$WORK/covprog.tabc.covmap" || {
  echo "CLI TEST FAIL: covmap missing '2 covprog.ta:0 grade'" >&2
  exit 1
}
grep -qE '^3 .*covprog\.ta:7 grade$' "$WORK/covprog.tabc.covmap" || {
  echo "CLI TEST FAIL: covmap missing statement '3 covprog.ta:7 grade'" >&2
  exit 1
}
grep -qE '^5 .*covprog\.ta:9 grade$' "$WORK/covprog.tabc.covmap" || {
  echo "CLI TEST FAIL: covmap missing match arm '5 covprog.ta:9 grade'" >&2
  exit 1
}
grep -qE '^6 .*covprog\.ta:10 grade$' "$WORK/covprog.tabc.covmap" || {
  echo "CLI TEST FAIL: covmap missing match arm '6 covprog.ta:10 grade'" >&2
  exit 1
}
grep -qE '^7 .*covprog\.ta:0 main$' "$WORK/covprog.tabc.covmap" || {
  echo "CLI TEST FAIL: covmap missing '7 covprog.ta:0 main'" >&2
  exit 1
}
# Empty-bodied fn: entry row with an empty file component (regression).
grep -qE '^13 :0 noop$' "$WORK/covprog.tabc.covmap" || {
  echo "CLI TEST FAIL: covmap missing '13 :0 noop' (empty fn body)" >&2
  exit 1
}
"$TAVM" -q "$WORK/covprog.tabc" | grep -qx '12'
grep -qE '^0 1$' /tmp/tinyactor-cov-cli-dump.txt || {
  echo "CLI TEST FAIL: dump missing '0 1' (leaf hit once)" >&2
  exit 1
}
grep -qE '^1 1$' /tmp/tinyactor-cov-cli-dump.txt || {
  echo "CLI TEST FAIL: dump missing '1 1' (leaf body hit once)" >&2
  exit 1
}
grep -qE '^5 1$' /tmp/tinyactor-cov-cli-dump.txt || {
  echo "CLI TEST FAIL: dump missing '5 1' (taken match arm hit once)" >&2
  exit 1
}
grep -qE '^6 0$' /tmp/tinyactor-cov-cli-dump.txt || {
  echo "CLI TEST FAIL: dump missing '6 0' (untaken match arm counted zero)" >&2
  exit 1
}
grep -qE '^7 1$' /tmp/tinyactor-cov-cli-dump.txt || {
  echo "CLI TEST FAIL: dump missing '7 1' (main hit once)" >&2
  exit 1
}
grep -qE '^99 1$' /tmp/tinyactor-cov-cli-dump.txt || {
  echo "CLI TEST FAIL: dump missing user hit id 99" >&2
  exit 1
}
echo "ok --cov instrument + covmap + dump"

# TA_COV_DUMP_DIR: the image dumps its own counters at exit, no injected
# call and no VM flag — the hook every compiler/test process shares.
mkdir -p "$WORK/exit-dumps"
TA_COV_DUMP_DIR="$WORK/exit-dumps" "$TAVM" "$WORK/covprog.tabc" >/dev/null
ls "$WORK/exit-dumps"/ >/dev/null 2>&1 || {
  echo "CLI TEST FAIL: TA_COV_DUMP_DIR produced no dump" >&2
  exit 1
}
grep -qE '^7 1$' "$WORK/exit-dumps"/* || {
  echo "CLI TEST FAIL: exit dump missing '7 1' (main hit once)" >&2
  exit 1
}
echo "ok TA_COV_DUMP_DIR exit dump"

# coverage_html.ta smoke: TA-native go-cover-style HTML report.
mkdir -p "$WORK/dumps"
cp /tmp/tinyactor-cov-cli-dump.txt "$WORK/dumps/prog.cov"
printf '%s\n' "$WORK/uncovered.ta" > "$WORK/sources.txt"
printf 'fn untouched() {\n  1\n}\n' > "$WORK/uncovered.ta"
"$TINYACTOR" run "$PROJECT_DIR/tools/coverage_html.ta" \
  "$WORK/covprog.tabc.covmap" "$WORK/dumps" "$WORK/coverage.html" "$WORK/misses.txt" "$WORK/sources.txt" || {
  echo "CLI TEST FAIL: coverage_html.ta crashed" >&2
  exit 1
}
grep -q 'class="l hit"' "$WORK/coverage.html" || {
  echo "CLI TEST FAIL: coverage html has no hit lines" >&2
  exit 1
}
grep -q 'class="l miss"' "$WORK/coverage.html" || {
  echo "CLI TEST FAIL: coverage html has no miss lines (missed match arm)" >&2
  exit 1
}
grep -q '<select id="source-select"' "$WORK/coverage.html" || {
  echo "CLI TEST FAIL: coverage html has no source selector" >&2
  exit 1
}
grep -q 'function select_source' "$WORK/coverage.html" || {
  echo "CLI TEST FAIL: coverage html has no source selection handler" >&2
  exit 1
}
grep -q "data-path=\"$WORK/uncovered.ta\"" "$WORK/coverage.html" || {
  echo "CLI TEST FAIL: coverage html omitted a zero-execution source from catalog" >&2
  exit 1
}
grep -A8 "data-path=\"$WORK/uncovered.ta\"" "$WORK/coverage.html" | grep -q 'class="l miss"' || {
  echo "CLI TEST FAIL: zero-execution source lines were not marked missed" >&2
  exit 1
}

if grep -q 'coverage-ta-sources.txt' "$PROJECT_DIR/Makefile"; then :; else
  echo "CLI TEST FAIL: coverage target does not create full-repo source catalog" >&2
  exit 1
fi

grep -q '^line ' "$WORK/misses.txt" || {
  echo "CLI TEST FAIL: misses.txt has no line entry (missed match arm)" >&2
  exit 1
}
echo "ok coverage html"

echo "CLI TEST PASS"