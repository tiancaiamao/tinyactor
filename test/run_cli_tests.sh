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
# 幂等：链接的 mtime 不变（ln -sfn 无条件跑会改，下游据此判定镜像过期）。
# 标准写法是紧贴式的 `-f%m`（BSD）→ 失败再 `-c%Y`（GNU）：GNU 的 `-f` 不吃参数，
# 写成 `-f '%m%c'` 会把格式串当文件、往 stdout 吐一份**文件系统块**（Free 字节
# 数随手变化），比较值就成了易变量，CI 并行写盘时误报「重写了链接」。
stamp_before=$(stat -f%m "$VDEPS/vendor/tinyactor" 2>/dev/null || stat -c%Y "$VDEPS/vendor/tinyactor")
(cd "$VDEPS" && "$TINYACTOR" vendor) | grep -q '已是最新' || {
  echo "CLI TEST FAIL: second tinyactor vendor was not a no-op" >&2
  exit 1
}
stamp_after=$(stat -f%m "$VDEPS/vendor/tinyactor" 2>/dev/null || stat -c%Y "$VDEPS/vendor/tinyactor")
[ "$stamp_before" = "$stamp_after" ] || {
  echo "CLI TEST FAIL: tinyactor vendor rewrote an already-correct link" >&2
  exit 1
}
echo "ok tinyactor vendor generates vendor/ from tinyactor.deps (idempotent)"

# 对齐：镜像 = 描述文件本身。删掉一行 → 对应的软链清掉（否则 import 会继续
# 命中一个已经不要的库），这条链留下的空壳一起收（`a/b` 没了，`a` 就不该留着）。
# 清理只动自己造的东西：真实文件/目录（有人手拷进 vendor/ 的）不删，但它们在
# 契约之外 —— `rm -rf vendor/` 之后同样会没。**不做全局空目录清扫**：clone 出
# 来的树内部有 `.git` 的一堆空目录，扫了就是把别人的盘搅了。
cat > "$VDEPS/tinyactor.deps" <<EOF
mylib	.
EOF
(cd "$VDEPS" && "$TINYACTOR" vendor) | grep -q '删旧链接 1，删空目录 0' || {
  echo "CLI TEST FAIL: tinyactor vendor kept a link whose deps line is gone" >&2
  exit 1
}
[ ! -e "$VDEPS/vendor/tinyactor" ] || {
  echo "CLI TEST FAIL: dropped deps line left a stale vendor/ link" >&2
  exit 1
}
mkdir -p "$VDEPS/vendor/handcopy/lib" "$VDEPS/vendor/emptydir"
printf 'kept\n' > "$VDEPS/vendor/handcopy/lib/real.ta"
ln -s real.ta "$VDEPS/vendor/handcopy/lib/alias.ta"
(cd "$VDEPS" && "$TINYACTOR" vendor) | grep -q '删旧链接 1，删空目录 0' || {
  echo "CLI TEST FAIL: tinyactor vendor did not mirror the deps file" >&2
  exit 1
}
[ ! -e "$VDEPS/vendor/handcopy/lib/alias.ta" ] || {
  echo "CLI TEST FAIL: a stray symlink under a real directory survived" >&2
  exit 1
}
[ -f "$VDEPS/vendor/handcopy/lib/real.ta" ] || {
  echo "CLI TEST FAIL: tinyactor vendor deleted a real file under vendor/" >&2
  exit 1
}
[ -d "$VDEPS/vendor/emptydir" ] || {
  echo "CLI TEST FAIL: tinyactor vendor touched an empty dir it did not orphan" >&2
  exit 1
}
rm -rf "$VDEPS/vendor/handcopy" "$VDEPS/vendor/emptydir"
# 嵌套树名删掉后，空出来的上级目录是「自己造的孤儿壳」，要收掉。
printf 'nest/inner\t.\n' >> "$VDEPS/tinyactor.deps"
(cd "$VDEPS" && "$TINYACTOR" vendor) >/dev/null 2>&1 || {
  echo "CLI TEST FAIL: tinyactor vendor failed on a nested tree" >&2
  exit 1
}
printf 'mylib\t.\n' > "$VDEPS/tinyactor.deps"
(cd "$VDEPS" && "$TINYACTOR" vendor) | grep -q '删旧链接 1，删空目录 1' || {
  echo "CLI TEST FAIL: tinyactor vendor left the shell of a dropped nested tree" >&2
  exit 1
}
[ ! -e "$VDEPS/vendor/nest" ] || {
  echo "CLI TEST FAIL: tinyactor vendor left the shell of a dropped nested tree" >&2
  exit 1
}
echo "ok tinyactor vendor mirrors the deps file (prunes links + empty dirs only)"

# 先清后建：树名在嵌套方向上改名（`a` ↔ `a/b`）都要能在 vendor/ 内部完成。
# 顺序若反过来，`mkdir -p` 会顺着旧软链走进来源仓库（在别人的仓库里留链接），
# 反向改名则被旧目录挡住报「已存在」——两条都不会报错退出，所以必须断言
# **来源目录没被动过**，光看 vendor/ 不够。
VEXT="$WORK/deptree"
mkdir -p "$VEXT/proj" "$VEXT/src1" "$VEXT/src2"
printf 'pub fn x() -> Int { 1 }\n' > "$VEXT/src1/x.ta"
printf 'pub fn y() -> Int { 2 }\n' > "$VEXT/src2/y.ta"
printf 'example.org\t../src1\n' > "$VEXT/proj/tinyactor.deps"
(cd "$VEXT/proj" && "$TINYACTOR" vendor) >/dev/null 2>&1 || {
  echo "CLI TEST FAIL: tinyactor vendor failed on a plain local tree" >&2
  exit 1
}
printf 'example.org/hello\t../src2\n' > "$VEXT/proj/tinyactor.deps"
(cd "$VEXT/proj" && "$TINYACTOR" vendor) >/dev/null 2>&1 || {
  echo "CLI TEST FAIL: tinyactor vendor failed to nest a tree (a -> a/b)" >&2
  exit 1
}
[ "$(readlink "$VEXT/proj/vendor/example.org/hello")" = "../../../src2" ] || {
  echo "CLI TEST FAIL: nested tree did not land inside vendor/" >&2
  exit 1
}
[ ! -e "$VEXT/src1/hello" ] || {
  echo "CLI TEST FAIL: tinyactor vendor wrote a link into the source tree" >&2
  exit 1
}
printf 'example.org\t../src1\n' > "$VEXT/proj/tinyactor.deps"
(cd "$VEXT/proj" && "$TINYACTOR" vendor) >/dev/null 2>&1 || {
  echo "CLI TEST FAIL: tinyactor vendor failed to un-nest a tree (a/b -> a)" >&2
  exit 1
}
[ "$(readlink "$VEXT/proj/vendor/example.org")" = "../../src1" ] || {
  echo "CLI TEST FAIL: un-nested tree did not become the top-level link" >&2
  exit 1
}
echo "ok tinyactor vendor renames trees on both nesting directions"

# 空镜像：描述文件一行树都没有（或全被删光）时，vendor/ 不存在也算「已是最新」。
# 这里最容易出的错是历史遗留的 `set -e` + 命令替换：find 报错 → 静默 exit 1。
printf '# 只剩注释\n' > "$VEXT/proj/tinyactor.deps"
rm -rf "$VEXT/proj/vendor"
(cd "$VEXT/proj" && "$TINYACTOR" vendor) | grep -q '已是最新（0 棵树' || {
  echo "CLI TEST FAIL: empty tinyactor.deps without vendor/ was not a no-op" >&2
  exit 1
}
echo "ok tinyactor vendor accepts an empty deps file (no vendor/ yet)"
# ---------- vendor：依赖的依赖（闭包合并）----------
# 模块解析只有**一个根**（入口向上第一个含 vendor/ 的目录），依赖自己的 vendor/
# 不被搜也不被看。所以 A→B→C 里 A 只写 B 时，C 必须被合并进来 —— 否则 A 编不过
# （`module not found: C`），而不是「C 恰好也在我机器上所以能跑」。
TDEP="$WORK/transdeps"
mkdir -p "$TDEP/a/lib" "$TDEP/b/lib" "$TDEP/c/lib" "$TDEP/dev/lib"
printf 'pub fn v() -> String { "c" }\n' > "$TDEP/c/lib/c.ta"
printf 'pub fn d() -> Int { 0 }\n' > "$TDEP/dev/lib/dev.ta"
cat > "$TDEP/b/lib/b.ta" <<'EOF'
import "example.org/c:lib/c"

pub fn f() -> String { c.v() }
EOF
# B 的 deps：一条真依赖、一条它的开发用树（行首 local）、一条指回 A（成环）。
# 只有第一条该进 A 的 vendor/：local 是「只给我自己构建用，不传播」。
cat > "$TDEP/b/tinyactor.deps" <<'EOF'
example.org/c	../c
local	example.org/dev	../dev
example.org/a	../a
EOF
printf 'example.org/b\t../b\n' > "$TDEP/a/tinyactor.deps"
cat > "$TDEP/a/main.ta" <<'EOF'
import "example.org/b:lib/b" as b

fn main() { print(b.f()) }
EOF
TDEP_OUT="$(cd "$TDEP/a" && "$TINYACTOR" vendor 2>&1)" || {
  echo "CLI TEST FAIL: tinyactor vendor failed to merge transitive deps" >&2
  printf '%s\n' "$TDEP_OUT" >&2
  exit 1
}
[ -L "$TDEP/a/vendor/example.org/c" ] || {
  echo "CLI TEST FAIL: transitive dep (A->B->C) did not reach A's vendor/" >&2
  exit 1
}
(cd "$TDEP/a" && "$TINYACTOR" run main.ta) | grep -qx 'c' || {
  echo "CLI TEST FAIL: project with a transitive dep does not build" >&2
  exit 1
}
[ ! -e "$TDEP/a/vendor/example.org/dev" ] || {
  echo "CLI TEST FAIL: a dependency's 'local' tree leaked into the consumer's vendor/" >&2
  exit 1
}
# B 的 deps 指回 A（成环）：读过的树根只读一次，收敛且不重复建。
[ "$(grep -c 'example.org/a' <<EOF
$TDEP_OUT
EOF
)" = "1" ] || {
  echo "CLI TEST FAIL: dependency cycle (A->B->A) was not visited exactly once" >&2
  printf '%s\n' "$TDEP_OUT" >&2
  exit 1
}
echo "ok tinyactor vendor merges transitive deps (local lines stay, cycles converge)"

# 同名不同源：两个依赖把同一个树名指到两份不同的代码。单根命名空间里只有一份
# vendor/<树名>，静默挑一个的后果是某个依赖被编译成别人的实现 —— 必须报错。
TCONF="$WORK/conflictdeps"
mkdir -p "$TCONF/d1/lib" "$TCONF/d2/lib" "$TCONF/aaa1/lib" "$TCONF/aaa2/lib"
printf 'pub fn v() -> Int { 1 }\n' > "$TCONF/aaa1/lib/a.ta"
printf 'pub fn v() -> Int { 2 }\n' > "$TCONF/aaa2/lib/a.ta"
printf 'import "aaa:lib/a"\n\npub fn f() -> Int { a.v() }\n' > "$TCONF/d1/lib/d.ta"
printf 'import "aaa:lib/a"\n\npub fn g() -> Int { a.v() }\n' > "$TCONF/d2/lib/d.ta"
printf 'aaa\t../aaa1\n' > "$TCONF/d1/tinyactor.deps"
printf 'aaa\t../aaa2\n' > "$TCONF/d2/tinyactor.deps"
printf 'example.org/d1\td1\nexample.org/d2\td2\n' > "$TCONF/tinyactor.deps"
TCONF_OUT="$WORK/conflict.out"
if (cd "$TCONF" && "$TINYACTOR" vendor) > "$TCONF_OUT" 2>&1; then
  echo "CLI TEST FAIL: two deps pinning one tree name to different sources did not fail" >&2
  exit 1
fi
grep -Fq '树名冲突 aaa' "$TCONF_OUT" || {
  echo "CLI TEST FAIL: conflict diagnostic does not name the contested tree" >&2
  cat "$TCONF_OUT" >&2
  exit 1
}
grep -Fq "$TCONF/d1/tinyactor.deps" "$TCONF_OUT" || {
  echo "CLI TEST FAIL: conflict diagnostic does not say who wanted what (d1 missing)" >&2
  cat "$TCONF_OUT" >&2
  exit 1
}
grep -Fq "$TCONF/d2/tinyactor.deps" "$TCONF_OUT" || {
  echo "CLI TEST FAIL: conflict diagnostic does not say who wanted what (d2 missing)" >&2
  cat "$TCONF_OUT" >&2
  exit 1
}
# 顶层写一行就解决：顶层优先（Go 的 replace 语义），并且说明它覆盖了谁。
printf 'example.org/d1\td1\nexample.org/d2\td2\naaa\taaa1\n' > "$TCONF/tinyactor.deps"
(cd "$TCONF" && "$TINYACTOR" vendor) > "$TCONF_OUT" 2>&1 || {
  echo "CLI TEST FAIL: a top-level line did not resolve the same-name conflict" >&2
  cat "$TCONF_OUT" >&2
  exit 1
}
grep -Fq 'note:' "$TCONF_OUT" || {
  echo "CLI TEST FAIL: top-level override happened without saying so" >&2
  cat "$TCONF_OUT" >&2
  exit 1
}
[ "$(readlink "$TCONF/vendor/aaa")" = "../aaa1" ] || {
  echo "CLI TEST FAIL: top-level override did not win" >&2
  exit 1
}
echo "ok tinyactor vendor reports same-name conflicts (top level wins, loudly)"

# 传递依赖 + git：B 的 deps 里写着一棵 git 树 C。B 得先 clone 下来才读得到它自己的
# tinyactor.deps，C 才可能被并进来 —— 「并表和落盘交替」就是为这一件事存在的。
# 用本地仓库当远端，不需要网。
TGIT="$WORK/gittrans"
mkdir -p "$TGIT/a" "$TGIT/upB/lib" "$TGIT/upC/lib"
printf 'pub fn v() -> String { "c-from-git" }\n' > "$TGIT/upC/lib/c.ta"
printf 'import "example.org/c2:lib/c"\n\npub fn f() -> String { c.v() }\n' > "$TGIT/upB/lib/b.ta"
printf 'example.org/c2\tgit+%s\n' "$TGIT/upC" > "$TGIT/upB/tinyactor.deps"
for r in "$TGIT/upC" "$TGIT/upB"; do
  (
    cd "$r" && git init -q . && git add -A \
      && git -c user.email=t@example.com -c user.name=t commit -qm v1
  ) >/dev/null 2>&1 || {
    echo "CLI TEST FAIL: could not build the transitive git upstream fixture" >&2
    exit 1
  }
done
printf 'example.org/b2\tgit+%s\n' "$TGIT/upB" > "$TGIT/a/tinyactor.deps"
cat > "$TGIT/a/main.ta" <<'EOF'
import "example.org/b2:lib/b" as b

fn main() { print(b.f()) }
EOF
(cd "$TGIT/a" && "$TINYACTOR" vendor) > "$TGIT/out" 2>&1 || {
  echo "CLI TEST FAIL: tinyactor vendor failed on a transitive git+ dep" >&2
  cat "$TGIT/out" >&2
  exit 1
}
[ -d "$TGIT/a/vendor/example.org/c2/.git" ] || {
  echo "CLI TEST FAIL: a cloned dep's own git+ dep was not merged (no merge/sync alternation)" >&2
  cat "$TGIT/out" >&2
  exit 1
}
(cd "$TGIT/a" && "$TINYACTOR" run main.ta) | grep -qx 'c-from-git' || {
  echo "CLI TEST FAIL: transitive git+ dep does not resolve imports" >&2
  exit 1
}
echo "ok tinyactor vendor merges deps of a cloned tree (merge/sync alternation)"
# git 来源：clone 成 vendor/ 下的真实目录（不再是软链）。整段用**本地**仓库当
# 远端，所以不需要网；也正因为是本地路径，「已就绪就不再连 git」这一点可以硬
# 验证 —— 把远端整个搬走再跑一遍，仍然得是 no-op。
#
# 断言一律「先落文件再 grep」：`vendor | grep -q` 在 grep 命中后立刻关管道，
# 上游 printf 吃 SIGPIPE，配上 set -o pipefail 就变成假失败。
GPROJ="$WORK/gitproj"
GUP="$WORK/upstream"
mkdir -p "$GPROJ" "$GUP/lib"
printf 'pub fn hello() -> String { "one" }\n' > "$GUP/lib/greet.ta"
(
  cd "$GUP" && git init -q . && git add -A \
    && git -c user.email=t@example.com -c user.name=t commit -qm v1 \
    && git tag v1 \
    && printf 'pub fn hello() -> String { "two" }\n' > lib/greet.ta \
    && git add -A && git -c user.email=t@example.com -c user.name=t commit -qm v2
) >/dev/null 2>&1 || {
  echo "CLI TEST FAIL: could not build the local git upstream fixture" >&2
  exit 1
}
cat > "$GPROJ/main.ta" <<'EOF'
import "pkg:lib/greet" as g
fn main() { print(g.hello()) }
EOF
gout="$GPROJ/vendor.out"
printf 'pkg\tgit+%s\n' "$GUP" > "$GPROJ/tinyactor.deps"
(cd "$GPROJ" && "$TINYACTOR" vendor) > "$gout" 2>&1 || {
  echo "CLI TEST FAIL: tinyactor vendor failed on a git+ source" >&2
  cat "$gout" >&2
  exit 1
}
grep -Fq 'vendor/pkg <-' "$gout" || {
  echo "CLI TEST FAIL: tinyactor vendor did not clone a git+ source" >&2
  cat "$gout" >&2
  exit 1
}
[ -d "$GPROJ/vendor/pkg/.git" ] && [ -f "$GPROJ/vendor/pkg/.git/tinyactor-clone" ] || {
  echo "CLI TEST FAIL: git+ source did not land as a real clone under vendor/" >&2
  exit 1
}
(cd "$GPROJ" && "$TINYACTOR" run main.ta) > "$gout" 2>&1
grep -qx 'two' "$gout" || {
  echo "CLI TEST FAIL: a cloned tree does not resolve imports" >&2
  cat "$gout" >&2
  exit 1
}
gstamp_before=$(stat -f%m "$GPROJ/vendor/pkg" 2>/dev/null || stat -c%Y "$GPROJ/vendor/pkg")
mv "$GUP" "$GUP.moved"
(cd "$GPROJ" && "$TINYACTOR" vendor) > "$gout" 2>&1
grep -Fq '已是最新' "$gout" || {
  echo "CLI TEST FAIL: second tinyactor vendor re-cloned (should be a no-op)" >&2
  cat "$gout" >&2
  exit 1
}
gstamp_after=$(stat -f%m "$GPROJ/vendor/pkg" 2>/dev/null || stat -c%Y "$GPROJ/vendor/pkg")
mv "$GUP.moved" "$GUP"
[ "$gstamp_before" = "$gstamp_after" ] || {
  echo "CLI TEST FAIL: tinyactor vendor rewrote an up-to-date clone" >&2
  exit 1
}
# 版本列：换成 tag v1 → 重新取，内容随之变成 v1 的。
printf 'pkg\tgit+%s\tv1\n' "$GUP" > "$GPROJ/tinyactor.deps"
(cd "$GPROJ" && "$TINYACTOR" vendor) > "$gout" 2>&1 || {
  echo "CLI TEST FAIL: tinyactor vendor failed on a pinned ref" >&2
  cat "$gout" >&2
  exit 1
}
(cd "$GPROJ" && "$TINYACTOR" run main.ta) > "$gout" 2>&1
grep -qx 'one' "$gout" || {
  echo "CLI TEST FAIL: the ref column did not pin the cloned tree" >&2
  cat "$gout" >&2
  exit 1
}
# 取不到（ref 不存在）时不许把已经好使的那棵弄坏。
printf 'pkg\tgit+%s\tno-such-ref\n' "$GUP" > "$GPROJ/tinyactor.deps"
if (cd "$GPROJ" && "$TINYACTOR" vendor) > "$gout" 2>&1; then
  echo "CLI TEST FAIL: tinyactor vendor accepted a non-existent ref" >&2
  exit 1
fi
grep -Fq '取不到' "$gout" || {
  echo "CLI TEST FAIL: no diagnostic for a non-existent ref" >&2
  cat "$gout" >&2
  exit 1
}
(cd "$GPROJ" && "$TINYACTOR" run main.ta) > "$gout" 2>&1
grep -qx 'one' "$gout" || {
  echo "CLI TEST FAIL: a failed fetch damaged the existing clone" >&2
  cat "$gout" >&2
  exit 1
}
# clone 树内部是**别人的盘**：上游仓库自带的软链不许被当成「过期的 vendor 链接」
# 清掉。清掉也不会被发现 —— clone 的幂等只看标记里的 url/ref，下一趟不会把它拉
# 回来，于是 vendor 与 pin 住的 commit 悄悄不一致。
printf 'pkg\tgit+%s\n' "$GUP" > "$GPROJ/tinyactor.deps"
(
  cd "$GUP" && printf 'real\n' > lib/real.ta && ln -s real.ta lib/alias.ta \
    && git add -A && git -c user.email=t@example.com -c user.name=t commit -qm link
) >/dev/null 2>&1 || {
  echo "CLI TEST FAIL: could not add a symlink to the upstream fixture" >&2
  exit 1
}
(cd "$GPROJ" && "$TINYACTOR" vendor) > "$gout" 2>&1 || {
  echo "CLI TEST FAIL: tinyactor vendor failed after the upstream gained a symlink" >&2
  cat "$gout" >&2
  exit 1
}
(cd "$GPROJ" && "$TINYACTOR" vendor) > "$gout" 2>&1
grep -Fq '已是最新' "$gout" || {
  echo "CLI TEST FAIL: second run after the upstream gained a symlink was not a no-op" >&2
  cat "$gout" >&2
  exit 1
}
[ -L "$GPROJ/vendor/pkg/lib/alias.ta" ] || {
  echo "CLI TEST FAIL: prune deleted a symlink that belongs to the cloned tree" >&2
  exit 1
}
gst=$(git -C "$GPROJ/vendor/pkg" status --porcelain)
[ -z "$gst" ] || {
  echo "CLI TEST FAIL: the cloned tree is not a clean checkout: $gst" >&2
  exit 1
}
echo "ok tinyactor vendor clones git+ sources (idempotent, offline, pinnable)"

# ---------- v2 锁定（#431）：tinyactor.lock ----------
# 入 git 的 lock 钉住解析结果：分支/tag 推进不跟，两台机器 vendor 出同一份内容。
# 上游用本地仓库 fixture（allowAnySHA1InWant 模拟 GitHub 对可达 sha 的放行）。
git -C "$GUP" config uploadpack.allowAnySHA1InWant true
# 此刻 deps = git+GUP 无 ref、盘上是旧代码时代的 clone（marker 无 lock 参与）。
# 新代码首次跑：无 lock → 解析当前 head → 写 lock。
(cd "$GPROJ" && "$TINYACTOR" vendor) > "$gout" 2>&1 || {
  echo "CLI TEST FAIL: tinyactor vendor failed writing the initial lock" >&2
  cat "$gout" >&2
  exit 1
}
[ -f "$GPROJ/tinyactor.lock" ] || {
  echo "CLI TEST FAIL: tinyactor vendor did not write tinyactor.lock" >&2
  exit 1
}
LOCKSHA=$(awk -F'\t' '$1 == "pkg" { print $4 }' "$GPROJ/tinyactor.lock")
[ "$LOCKSHA" = "$(git -C "$GUP" rev-parse HEAD)" ] || {
  echo "CLI TEST FAIL: lock sha is not the resolved head" >&2
  exit 1
}
# 可复现的核心：上游推进之后，vendor 不跟；删掉 vendor/ 重建也恢复锁定的 commit。
(
  cd "$GUP" && printf 'pub fn hello() -> String { "three" }\n' > lib/greet.ta \
    && git add -A && git -c user.email=t@example.com -c user.name=t commit -qm v3
) >/dev/null 2>&1
(cd "$GPROJ" && "$TINYACTOR" vendor) > "$gout" 2>&1
grep -Fq '已是最新' "$gout" || {
  echo "CLI TEST FAIL: vendor followed an upstream commit the lock does not pin" >&2
  cat "$gout" >&2
  exit 1
}
rm -rf "$GPROJ/vendor/pkg"
(cd "$GPROJ" && "$TINYACTOR" vendor) > "$gout" 2>&1 || {
  echo "CLI TEST FAIL: vendor failed rebuilding from the lock" >&2
  cat "$gout" >&2
  exit 1
}
(cd "$GPROJ" && "$TINYACTOR" run main.ta) > "$gout" 2>&1
grep -qx 'two' "$gout" || {
  echo "CLI TEST FAIL: rebuild did not restore the locked commit (got: $(cat "$gout"))" >&2
  exit 1
}
[ "$(awk -F'\t' '$1 == "pkg" { print $4 }' "$GPROJ/tinyactor.lock")" = "$LOCKSHA" ] || {
  echo "CLI TEST FAIL: rebuild rewrote the lock" >&2
  exit 1
}
echo "ok tinyactor.lock pins the resolved commit (upstream drift is not followed)"
# deps 第三列直接写 40-hex sha：浅取那一个 commit。
V1SHA=$(git -C "$GUP" rev-parse v1)
printf 'pkg\tgit+%s\t%s\n' "$GUP" "$V1SHA" > "$GPROJ/tinyactor.deps"
(cd "$GPROJ" && "$TINYACTOR" vendor) > "$gout" 2>&1 || {
  echo "CLI TEST FAIL: tinyactor vendor failed on a sha ref" >&2
  cat "$gout" >&2
  exit 1
}
(cd "$GPROJ" && "$TINYACTOR" run main.ta) > "$gout" 2>&1
grep -qx 'one' "$gout" || {
  echo "CLI TEST FAIL: a sha ref did not pin the tree to that commit" >&2
  exit 1
}
(cd "$GPROJ" && "$TINYACTOR" vendor) > "$gout" 2>&1
grep -Fq '已是最新' "$gout" || {
  echo "CLI TEST FAIL: second vendor after a sha ref was not a no-op" >&2
  cat "$gout" >&2
  exit 1
}
echo "ok deps ref column accepts a commit sha (shallow fetch by commit)"
# ref 改了 = 人换了意图：lock 不匹配 → 自动重解析并改写 lock（master = v3 head）。
printf 'pkg\tgit+%s\tmaster\n' "$GUP" > "$GPROJ/tinyactor.deps"
(cd "$GPROJ" && "$TINYACTOR" vendor) > "$gout" 2>&1 || {
  echo "CLI TEST FAIL: tinyactor vendor failed on a changed ref" >&2
  cat "$gout" >&2
  exit 1
}
(cd "$GPROJ" && "$TINYACTOR" run main.ta) > "$gout" 2>&1
grep -qx 'three' "$gout" || {
  echo "CLI TEST FAIL: a changed ref did not re-resolve (got: $(cat "$gout"))" >&2
  exit 1
}
M3SHA=$(git -C "$GUP" rev-parse HEAD)
[ "$(awk -F'\t' '$1 == "pkg" { print $3 }' "$GPROJ/tinyactor.lock")" = "master" ] || {
  echo "CLI TEST FAIL: lock did not record the new ref" >&2
  exit 1
}
# ref 没变但上游推进：vendor 不跟；--update 显式刷新。
(
  cd "$GUP" && printf 'pub fn hello() -> String { "four" }\n' > lib/greet.ta \
    && git add -A && git -c user.email=t@example.com -c user.name=t commit -qm v4
) >/dev/null 2>&1
(cd "$GPROJ" && "$TINYACTOR" vendor) > "$gout" 2>&1
(cd "$GPROJ" && "$TINYACTOR" run main.ta) > "$gout" 2>&1
grep -qx 'three' "$gout" || {
  echo "CLI TEST FAIL: vendor followed upstream after lock pinning (got: $(cat "$gout"))" >&2
  exit 1
}
(cd "$GPROJ" && "$TINYACTOR" vendor --update pkg) > "$gout" 2>&1 || {
  echo "CLI TEST FAIL: vendor --update failed" >&2
  cat "$gout" >&2
  exit 1
}
(cd "$GPROJ" && "$TINYACTOR" run main.ta) > "$gout" 2>&1
grep -qx 'four' "$gout" || {
  echo "CLI TEST FAIL: --update did not move to the ref's new head (got: $(cat "$gout"))" >&2
  exit 1
}
[ "$(awk -F'\t' '$1 == "pkg" { print $4 }' "$GPROJ/tinyactor.lock")" = "$(git -C "$GUP" rev-parse master)" ] || {
  echo "CLI TEST FAIL: --update did not refresh the lock" >&2
  exit 1
}
if (cd "$GPROJ" && "$TINYACTOR" vendor --update no-such-tree) > "$gout" 2>&1; then
  echo "CLI TEST FAIL: --update accepted a tree that is not in the closure" >&2
  exit 1
fi
grep -Fq '闭包里没有 git 树' "$gout" || {
  echo "CLI TEST FAIL: no diagnostic for --update with an unknown tree" >&2
  cat "$gout" >&2
  exit 1
}
echo "ok vendor --update re-resolves explicitly and rejects unknown trees"
# lock 里是坏 sha：浅取与完整 clone 都到不了那个 commit → 报错且旧 clone 不坏。
cp "$GPROJ/tinyactor.lock" "$GPROJ/tinyactor.lock.bak"
# lock 的 url 列是剥离 git+ 后的拼写（与 deps_entries 的规范化一致）。
# deps 保持 master：ref 与 lock 行一致才会命中坏 sha（ref 不一致 = 人换了
# 意图，走自动重解析，不经过这条报错路径）。
printf 'pkg\t%s\tmaster\t%040d\n' "$GUP" 0 > "$GPROJ/tinyactor.lock"
rm -rf "$GPROJ/vendor/pkg"
if (cd "$GPROJ" && "$TINYACTOR" vendor) > "$gout" 2>&1; then
  echo "CLI TEST FAIL: tinyactor vendor accepted a bogus lock sha" >&2
  exit 1
fi
grep -Fq '取不到' "$gout" || {
  echo "CLI TEST FAIL: no diagnostic for a bogus lock sha" >&2
  cat "$gout" >&2
  exit 1
}
mv "$GPROJ/tinyactor.lock.bak" "$GPROJ/tinyactor.lock"
(cd "$GPROJ" && "$TINYACTOR" vendor) > "$gout" 2>&1
(cd "$GPROJ" && "$TINYACTOR" run main.ta) > "$gout" 2>&1
grep -qx 'four' "$gout" || {
  echo "CLI TEST FAIL: a failed bogus-sha vendor left the tree unusable" >&2
  exit 1
}
echo "ok a bogus lock sha is rejected without damaging the existing tree"

# 无树名 = 全部 git 树：整个 lock 重解析，锁随之刷新（同理不得碰本地目录树）。
(
  cd "$GUP" && printf 'pub fn hello() -> String { "five" }\n' > lib/greet.ta \
    && git add -A && git -c user.email=t@example.com -c user.name=t commit -qm v5
) >/dev/null 2>&1
(cd "$GPROJ" && "$TINYACTOR" vendor) > "$gout" 2>&1
(cd "$GPROJ" && "$TINYACTOR" run main.ta) > "$gout" 2>&1
grep -qx 'four' "$gout" || {
  echo "CLI TEST FAIL: bare vendor followed upstream (got: $(cat "$gout"))" >&2
  exit 1
}
(cd "$GPROJ" && "$TINYACTOR" vendor --update) > "$gout" 2>&1 || {
  echo "CLI TEST FAIL: bare vendor --update failed" >&2
  cat "$gout" >&2
  exit 1
}
(cd "$GPROJ" && "$TINYACTOR" run main.ta) > "$gout" 2>&1
grep -qx 'five' "$gout" || {
  echo "CLI TEST FAIL: bare --update did not re-resolve (got: $(cat "$gout"))" >&2
  exit 1
}
[ "$(awk -F'\t' '$1 == "pkg" { print $4 }' "$GPROJ/tinyactor.lock")" = "$(git -C "$GUP" rev-parse master)" ] || {
  echo "CLI TEST FAIL: bare --update did not refresh the lock" >&2
  exit 1
}
echo "ok bare vendor --update re-resolves every git tree"

# lock 对账：闭包里已经没了的 git 树，lock 行跟着收走——vendor/ 在与不在都
# 一样（它就是生成物，fresh checkout / `git clean -xdf` 之后不在了）。
printf 'ghost\t%s\tmaster\t%s\n' "$GUP" "$(git -C "$GUP" rev-parse master)" >> "$GPROJ/tinyactor.lock"
(cd "$GPROJ" && "$TINYACTOR" vendor) > "$gout" 2>&1
if grep -q '^ghost' "$GPROJ/tinyactor.lock"; then
  echo "CLI TEST FAIL: stale lock row survived a vendor run" >&2
  exit 1
fi
printf 'ghost\t%s\tmaster\t%s\n' "$GUP" "$(git -C "$GUP" rev-parse master)" >> "$GPROJ/tinyactor.lock"
rm -rf "$GPROJ/vendor"
(cd "$GPROJ" && "$TINYACTOR" vendor) > "$gout" 2>&1
if grep -q '^ghost' "$GPROJ/tinyactor.lock"; then
  echo "CLI TEST FAIL: stale lock row survived when vendor/ was absent" >&2
  exit 1
fi
echo "ok stale lock rows are reconciled away (with and without vendor/)"


# 形态切换与清理：同一棵树从 git 换成本地目录要重建（不能留着上一版的目录），
# 描述文件里没有的 clone 目录要被清掉；没有标记的真实目录（人塞进来的）不碰。
mkdir -p "$GPROJ/mylib"
printf 'pub fn hey() -> String { "hey" }\n' > "$GPROJ/mylib/hey.ta"
printf 'pkg\tmylib\n' > "$GPROJ/tinyactor.deps"
(cd "$GPROJ" && "$TINYACTOR" vendor) > "$gout" 2>&1 || {
  echo "CLI TEST FAIL: tinyactor vendor failed on the git -> local switch" >&2
  cat "$gout" >&2
  exit 1
}
[ -L "$GPROJ/vendor/pkg" ] || {
  echo "CLI TEST FAIL: git -> local switch left the old clone in place" >&2
  exit 1
}
if grep -q '^pkg' "$GPROJ/tinyactor.lock"; then
  echo "CLI TEST FAIL: git -> local switch left a stale lock row" >&2
  exit 1
fi
printf 'pkg\tgit+%s\n' "$GUP" > "$GPROJ/tinyactor.deps"
(cd "$GPROJ" && "$TINYACTOR" vendor) > "$gout" 2>&1 || {
  echo "CLI TEST FAIL: tinyactor vendor failed on the local -> git switch" >&2
  cat "$gout" >&2
  exit 1
}
[ -d "$GPROJ/vendor/pkg/.git" ] || {
  echo "CLI TEST FAIL: local -> git switch left the old symlink in place" >&2
  exit 1
}
mkdir -p "$GPROJ/vendor/handmade"
printf 'pub fn old() -> Int { 9 }\n' > "$GPROJ/vendor/handmade/old.ta"
printf 'pkg\tgit+%s\nhandmade\tgit+%s\n' "$GUP" "$GUP" > "$GPROJ/tinyactor.deps"
if (cd "$GPROJ" && "$TINYACTOR" vendor) > "$gout" 2>&1; then
  echo "CLI TEST FAIL: tinyactor vendor overwrote a hand-made directory" >&2
  exit 1
fi
grep -Fq '不是本工具 clone 的' "$gout" || {
  echo "CLI TEST FAIL: no diagnostic when a hand-made directory is in the way" >&2
  cat "$gout" >&2
  exit 1
}
[ -f "$GPROJ/vendor/handmade/old.ta" ] || {
  echo "CLI TEST FAIL: tinyactor vendor deleted a hand-made directory" >&2
  exit 1
}
rm -rf "$GPROJ/vendor/handmade"
printf 'pkg\tgit+%s\n' "$GUP" > "$GPROJ/tinyactor.deps"
(cd "$GPROJ" && "$TINYACTOR" vendor) > "$gout" 2>&1
printf '# 空\n' > "$GPROJ/tinyactor.deps"
(cd "$GPROJ" && "$TINYACTOR" vendor) > "$gout" 2>&1
grep -Fq '删旧链接 1' "$gout" || {
  echo "CLI TEST FAIL: dropped git line left the clone under vendor/" >&2
  cat "$gout" >&2
  exit 1
}
[ ! -e "$GPROJ/vendor/pkg" ] || {
  echo "CLI TEST FAIL: dropped git line left the clone under vendor/" >&2
  exit 1
}
echo "ok tinyactor vendor switches source kinds and prunes dropped clones"

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
# 单列 = 远端（树名即仓库路径）。`mylib` 没有 host/path 的形状 → 拒绝，免得
# 变成一个 https://mylib 的瞎 clone。
vdep_fail 'mylib' "才表示远端"
# 版本列只对 git+ 有意义：本机目录没有版本概念，带了就是写错。
vdep_fail 'mylib	.  v1.0.0' "不能带版本"
vdep_fail '../evil	.' "非法树名"
vdep_fail './mylib	.' "非法树名"
vdep_fail 'mylib/	.' "非法树名"
# 每棵树是一条链接，`a` 与 `a/b` 在 vendor/ 下落不下：放任的话 mkdir 会顺着
# `a` 那条链接走进来源仓库，在别人的仓库里建链接。
vdep_fail 'mylib	.
mylib/sub	.' "互为前缀"
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