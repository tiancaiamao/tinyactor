#!/usr/bin/env bash
# run_cli_tests.sh — CLI behavior from outside the TinyActor checkout.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TINYACTOR="${TINYACTOR:-$PROJECT_DIR/tinyactor}"
TAVM="${TAVM:-$PROJECT_DIR/tavm}"
TAVM_DRIVER_BC="$PROJECT_DIR/lib/bootstrap.tabc"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

[ -x "$TINYACTOR" ] || { echo "CLI TEST FAIL: tinyactor not executable: $TINYACTOR" >&2; exit 1; }
[ -x "$TAVM" ] || { echo "CLI TEST FAIL: tavm not executable: $TAVM" >&2; exit 1; }
[ -s "$TAVM_DRIVER_BC" ] || { echo "CLI TEST FAIL: lisp backend driver missing: $TAVM_DRIVER_BC" >&2; exit 1; }

# lisp 编译半程：与 tinyactor run 的 run_tavm 同参布局（driver 编 .tabc）。
# 测试直接喂 driver 是为了在 .tabc 之上测 VM 级 flag（--profile /
# TA_DUMP_INTERNS）——tinyactor run 不透传它们。
compile_lisp() {
    local src="$1" out="$2"
    local cache="${TA_SIG_CACHE_DIR:-$PROJECT_DIR/.build/modules}"
    (cd "$PROJECT_DIR" && env -u TA_ACTOR_HEAP "$TAVM" -q "$TAVM_DRIVER_BC" "$src" "$out" "$cache")
}

cat > "$WORK/fs_cli.ta" <<'EOF'
import fs
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
import os
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
import os
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
# ---------- 消费项目自带 vendor 树（issue #368） ----------
# 模块解析基准 = 调用方项目根（CLI 传 TA_PROJECT_ROOT=$PWD，driver.ta 的
# vendor_roots）：fixture test/consumer 是一个自带 vendor 树的消费项目 ——
# 自指 identity 符号链接、项目独有的裸名模块、影蔽安装树同名者的项目副本。
# 旧行为（解析基准 = 编译进程 cwd = 安装树）下这些全是 module not found。
CONSUMER="$PROJECT_DIR/test/consumer"
[ -e "$CONSUMER/vendor/example.org/consumer" ] || {
  echo "CLI TEST FAIL: consumer fixture symlink missing: $CONSUMER" >&2
  exit 1
}
consumer_run="$(cd "$CONSUMER" && "$TINYACTOR" run app/main.ta)"
consumer_check() {
  printf '%s\n' "$consumer_run" | grep -qx "$1" || {
    echo "CLI TEST FAIL: consumer vendor tree: $2 (missing line: $1)" >&2
    printf '%s\n' "$consumer_run" >&2
    exit 1
  }
}
consumer_check 'consumer: project-local module' 'self-referential identity module not resolved'
consumer_check 'consumerlib: from the project vendor tree' 'project-only bare-name module not resolved'
consumer_check 'json: from the project vendor tree' 'project copy did not win over the installed json'
consumer_check "hello from the consumer project's vendor tree" 'project copy did not win over the installed example.org/hello'
consumer_check 'CONSUMER VENDOR OK' 'run never reached the pass marker'

# build 是独立出口（run = build + tavm）：同一基准，产物直接可跑。
(cd "$CONSUMER" && "$TINYACTOR" build app/main.ta "$WORK/consumer.tabc")
[ -s "$WORK/consumer.tabc" ] || { echo "CLI TEST FAIL: consumer build wrote no image" >&2; exit 1; }
"$TAVM" -q "$WORK/consumer.tabc" | grep -qx 'CONSUMER VENDOR OK' || {
  echo "CLI TEST FAIL: consumer-built image lost the project modules" >&2
  exit 1
}
echo "ok consumer project vendor tree (project root first, identity + bare names)"

# 默认等价性：没有 vendor 树的目录里，裸名标准库仍落回安装树（根表第 2 项
# = 今天的 cwd 相对 "vendor/"），老用户面零变化。
mkdir -p "$WORK/novendor"
cat > "$WORK/novendor/fallback.ta" <<'EOF'
import str
fn main() { print(str.concat("STDLIB ", "FALLBACK OK")) }
EOF
(cd "$WORK/novendor" && "$TINYACTOR" run fallback.ta) | grep -qx 'STDLIB FALLBACK OK' || {
  echo "CLI TEST FAIL: vendor-less project lost the installed stdlib fallback" >&2
  exit 1
}
echo "ok vendor-less project falls back to the installed stdlib"

# ---------- TA 覆盖率工具链（--cov / covmap / dump / html） ----------
# Coverage instrumentation: `build --cov` rewrites fn entries AND every
# statement / match-arm body to cov.hit(k) and emits a <out>.covmap side
# table ("k file:line name" rows, line 0 = fn entry). Running the
# instrumented image and dumping must reproduce the covmap ids with the
# right counts (join-ability is the whole contract), including missed
# statements reporting count 0.
cat > "$WORK/covprog.ta" <<'EOF'
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
grep -qE '^3 .*covprog\.ta:6 grade$' "$WORK/covprog.tabc.covmap" || {
  echo "CLI TEST FAIL: covmap missing statement '3 covprog.ta:6 grade'" >&2
  exit 1
}
grep -qE '^5 .*covprog\.ta:8 grade$' "$WORK/covprog.tabc.covmap" || {
  echo "CLI TEST FAIL: covmap missing match arm '5 covprog.ta:8 grade'" >&2
  exit 1
}
grep -qE '^6 .*covprog\.ta:9 grade$' "$WORK/covprog.tabc.covmap" || {
  echo "CLI TEST FAIL: covmap missing match arm '6 covprog.ta:9 grade'" >&2
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