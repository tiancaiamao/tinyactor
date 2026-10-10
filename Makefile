CC ?= cc
UNAME_S := $(shell uname -s)

# ============================================================
# Shared library for dynamic C module loading
# macOS uses .dylib, Linux uses .so
#
# Each module is built per sanitizer configuration
# (lib/demo.dylib / lib/demo_asan.dylib / lib/demo_tsan.dylib, ...), so a
# sanitizer build never overwrites — or leaves behind — the plain module
# that the VM dlopens at startup. (A TSAN-instrumented
# lib/demo.dylib used to abort the plain VM with "Interceptors are not
# working", failing every test.)
# ============================================================
ifeq ($(UNAME_S),Darwin)
HTTP_EXT = dylib
else
HTTP_EXT = so
endif

# ============================================================
# Sanitizer support
#   ASAN=1 make lispvm  → build with AddressSanitizer
#   TSAN=1 make lispvm  → build with ThreadSanitizer
# ============================================================
ifdef ASAN
  ifdef TSAN
    $(error ASAN=1 and TSAN=1 are mutually exclusive)
  endif
  override SAN := asan
  override SAN_CFLAGS  := -fsanitize=address -fno-omit-frame-pointer -O1 -g -DTA_MOD_TAG=asan
  override SAN_LDFLAGS := -fsanitize=address
else ifdef TSAN
  override SAN := tsan
  override SAN_CFLAGS  := -fsanitize=thread -fno-omit-frame-pointer -O1 -g -DTA_MOD_TAG=tsan
  override SAN_LDFLAGS := -fsanitize=thread
endif

# Coverage mode (COV=1): build with clang line/instr coverage so the test
# suite can be measured with llvm-profdata + llvm-cov (see "coverage" target).
# Like the sanitizer builds, the instrumented binary is a separate
# lispvm (obj_cov) that loads its own lib/demo_cov module and never
# touches the plain build's modules.
ifdef COV
  ifdef SAN
    $(error COV=1 is mutually exclusive with ASAN=1 / TSAN=1)
  endif
  override COV_TAG     := cov
  override COV_CFLAGS  := -fprofile-instr-generate -fcoverage-mapping -fno-omit-frame-pointer -O1 -g -DTA_MOD_TAG=$(COV_TAG)
  override COV_LDFLAGS := -fprofile-instr-generate
  # Compile with the clang from the same LLVM install that provides
  # llvm-profdata/llvm-cov. Apple clang writes raw profile format v8 while
  # LLVM >= 17 tools expect v10, and raw profiles are NOT backward
  # readable — a mismatched pair dies with "raw profile version mismatch".
  # An explicit CC= on the command line still wins (plain assignment).
  LLVM_BIN := $(dir $(shell command -v llvm-profdata 2>/dev/null))
  ifneq ($(LLVM_BIN),)
    CC = $(LLVM_BIN)clang
  endif
endif

ifdef COV
  OBJ_DIR  := obj_cov
  CFLAGS    = -Wall -Wextra -std=c99 -I. $(COV_CFLAGS)
  LDLIBS    = $(COV_LDFLAGS)
  # C modules (lib/demo.c, lib/math.c, ...) are NOT instrumented under COV:
  # a dlopen'd library pulls in its own profile runtime and its counters
  # never flush into the main executable's merged profraw. They keep the
  # module tag (so the cov build loads the _cov variant) but plain flags.
  MOD_CFLAGS = -Wall -Wextra -std=c99 -O2 -I. -DTA_MOD_TAG=$(COV_TAG)
  MOD_LDLIBS =
else ifdef SAN
  OBJ_DIR  := obj_$(SAN)
  CFLAGS    = -Wall -Wextra -std=c99 -I. $(SAN_CFLAGS)
  LDLIBS    = $(SAN_LDFLAGS)
  MOD_CFLAGS = $(CFLAGS)
  MOD_LDLIBS = $(LDLIBS)
else
  OBJ_DIR  := src
  CFLAGS    = -Wall -Wextra -std=c99 -O2 -I.
  LDLIBS    =
  MOD_CFLAGS = $(CFLAGS)
  MOD_LDLIBS = $(LDLIBS)
endif

# Shared module output — one per build config (plain / _asan / _tsan / _cov),
# so a sanitizer/coverage build never overwrites the module the plain VM loads.
DEMO_LIB := lib/demo$(COV_TAG:%=_%)$(SAN:%=_%).$(HTTP_EXT)
MATH_LIB := lib/math$(COV_TAG:%=_%)$(SAN:%=_%).$(HTTP_EXT)
TIME_LIB := lib/time$(COV_TAG:%=_%)$(SAN:%=_%).$(HTTP_EXT)
BUFFER_LIB := lib/buffer$(COV_TAG:%=_%)$(SAN:%=_%).$(HTTP_EXT)
PROCESS_LIB := lib/process$(COV_TAG:%=_%)$(SAN:%=_%).$(HTTP_EXT)
SEXP_LIB := lib/sexp$(COV_TAG:%=_%)$(SAN:%=_%).$(HTTP_EXT)

ifdef GC_DEBUG
  CFLAGS += -DGC_DEBUG=1
endif

# ============================================================
# OpenSSL for the static tls module (stdlib-port-plan Phase 6 #1).
#   macOS: Homebrew openssl@3 (keg-only, not on the default include/lib
#          search path) — fall back to pkg-config openssl when brew (or
#          the formula) is absent.
#   Linux: system libssl-dev via pkg-config, or plain -lssl -lcrypto.
# The module is compiled into the VM itself, so these flags apply to the
# whole VM build (a dlopen'd tls dylib would have to link OpenSSL per
# sanitizer variant instead — see src/tls.c header for the static-vs-
# dylib rationale).
# ============================================================
ifeq ($(UNAME_S),Darwin)
OPENSSL_PREFIX := $(shell brew --prefix openssl@3 2>/dev/null)
endif
ifneq ($(OPENSSL_PREFIX),)
TLS_CFLAGS := -I$(OPENSSL_PREFIX)/include
TLS_LIBS   := -L$(OPENSSL_PREFIX)/lib -lssl -lcrypto
else
TLS_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
TLS_LIBS   := $(shell pkg-config --libs openssl 2>/dev/null)
ifeq ($(TLS_LIBS),)
TLS_LIBS := -lssl -lcrypto
endif
ifeq ($(shell pkg-config --exists openssl 2>/dev/null && echo y),)
$(warning TLS: no OpenSSL dev files found (brew openssl@3 / pkg-config \
openssl both missed). Linking plain -lssl -lcrypto and hoping they are \
on the default search path; if the link fails, install them first — \
Debian/Ubuntu: apt-get install libssl-dev pkg-config, Fedora: \
dnf install openssl-devel pkgconf-pkg-config)
endif
endif
CFLAGS += $(TLS_CFLAGS)
LDLIBS += $(TLS_LIBS)


# Linux needs -ldl for dlopen/dlsym; macOS has it in libSystem
ifneq ($(UNAME_S),Darwin)
LDLIBS += -ldl
LDLIBS += -lm
# glibc hides POSIX declarations under strict -std=c99 (__STRICT_ANSI__),
# e.g. gethostname in unistd.h — gcc's gnu defaults masked this until the
# clang-based COV build hit it on Ubuntu CI. The VM uses POSIX APIs
# throughout (net/file/os/tls), so request them explicitly. No-op on BSD.
CFLAGS += -D_DEFAULT_SOURCE
endif
# Export symbols for dynamically loaded modules (needed on Linux for dlopen)
ifeq ($(UNAME_S),Linux)
RDYNAMIC = -Wl,--export-dynamic
else
# macOS: allow shared lib to have unresolved symbols resolved at load time
UNDEF_OK = -undefined dynamic_lookup
endif

SRC     = src/val.c src/vm.c src/builtin.c src/scheduler.c src/timer.c src/gc.c src/api.c src/net.c src/tls.c src/file.c src/os.c src/buf.c src/cov.c src/str.c src/num.c src/encoding.c src/random.c src/prof.c
OBJ     = $(SRC:src/%.c=$(OBJ_DIR)/%.o)

.PHONY: all clean test test-basic test-gc test-actor test-module test-compiler \
        test-bootstrap test-example test-cli test-gc-asan test-gc-tsan \
        test-asan test-tsan test-cov coverage test-1m-actor test-gc-long \
        bootstrap benchmark benchmark-regression \
        benchmark-clean fmt fmt-version-check kernfuzz-fast kernfuzz-freeze-tc \
        test-fmt-guard \
        kernfuzz-nightly kernfuzz-snapshot-check

# Default build ships the complete C-module set: a bare `make clean; make`
# must leave a runtime where scripts can actually call demo/math/time/
# buffer/process (the dylibs are lazy-loaded; a missing one makes the call
# silently push nil instead of erroring).
all: lispvm $(DEMO_LIB) $(MATH_LIB) $(TIME_LIB) $(BUFFER_LIB) $(PROCESS_LIB) $(SEXP_LIB)

# lispvm: the VM binary — lisp interpreter loop + main in vm-demo/lisp/lispvm.c,
# linked against the TA runtime objects (values, heap, GC, scheduler, C modules).
LISPVM_OBJ = $(OBJ)
.PHONY: lispvm
# 原子重链（-o tmp && mv）：lispvm 是 .PHONY，`make fmt` 等任何嵌套 make 都会
# 触发重链。GNU ld 的 -o 会先在目标路径建 0644 文件、链接完才 chmod——窗口内
# 并行 make -j8 test 的测试进程 exec 它得到 "Permission denied" (exit 126)
# （PR #274 ubuntu 首跑：fmt-guard 嵌套 make fmt × 基本类测试并发踩中）。mktemp
# 先建后 rm 再给 cc：mktemp 文件是 0600，留着会让产物永久不可执行。
lispvm: $(LISPVM_OBJ) vm-demo/lisp/lispvm.c
	@tmp=$$(mktemp lispvm.XXXXXX) || exit 1; rm -f "$$tmp"; \
	$(CC) $(CFLAGS) $(RDYNAMIC) -o "$$tmp" vm-demo/lisp/lispvm.c $(LISPVM_OBJ) -lpthread $(LDLIBS) || { rm -f "$$tmp"; exit 1; }; \
	rm -rf "$$tmp.dSYM"; mv -f "$$tmp" $@

# lispvm_asan：kernfuzz morph lisp 臂（tools/kernfuzz/lisparm.py）的 ASan
# 底座。独立输出名——test-asan 故意用 ASAN=1 make lispvm 覆盖 plain 版，
# kernfuzz 不得搅动 ./lispvm（一条分支一个问题，不顺手动它）。
# 仅 SAN=asan 配置下有此规则：裸 `make lispvm_asan` 无规则报错响亮，
# 不会静默建出无插桩的赝品。
ifeq ($(SAN),asan)
lispvm_asan: $(LISPVM_OBJ) vm-demo/lisp/lispvm.c
	$(CC) $(CFLAGS) $(RDYNAMIC) -o $@ vm-demo/lisp/lispvm.c $(LISPVM_OBJ) -lpthread $(LDLIBS)
endif

# ============================================================
# Driver 运行期前置 —— 单一事实来源（invariant）
#
# INVARIANT：任何会在 recipe 里执行 driver 编译半程的 make 目标——
#   `./lispvm -q vm-demo/lisp/boot/backend_driver.bc ...`（bootstrap /
#   boot-backend-driver / .bc file 目标）或 `./tinyactor run|build|fmt`
#   （test-* / kernfuzz-* / lisp-gate / benchmark / fmt*）——
# prerequisite 必须带上 $(DRIVER_DEPS)：直接引用本变量（TEST_DEPS /
# boot-backend-driver / lisp-gate / bootstrap / kernfuzz-snapshot-check），
# 或传递依赖携带它的目标——.bc 文件目标（其 prerequisite 含
# $(DRIVER_DEPS)，见下）与 phony boot-backend-driver——跑 driver 必有
# .bc，这条传递依赖总是成立（test-asan / kernfuzz-fast / fmt /
# benchmark 等即此类）。新增这类目标时引用本变量或上述目标，不要再逐处
# 粘贴 lispvm + $(SEXP_MODS)（人肉记得加已被证明不可靠）。
#
# 丢了它的代价不是响亮报错，而是静默劣化：driver 运行期按模块名 dlopen
# lib/sexp.$EXT（lispvm.c find_cfunc_autoload；lib/sexp.c = S-expr reader，
# cfunc/源码解析用）。模块缺失时 driver 不报错，改走"无限分配"路径一路
# 吃到 actor arena 上限，报出来的是误导性的：
#   tavm: fatal: actor heap arena exhausted (stack outgrew the arena)
# 本地手 build 过就绿、fresh clone 才红——coverage-c 自 #275 起 83 语料
# 全红即此因（kernfuzz-snapshot-check 把 $(TARGET) 换成 lispvm 时连带
# 丢了这条传递依赖）。
#
# 展开时机坑：GNU make 对显式规则的 prerequisite 是读取时立即展开的，
# 所以本块（SEXP_MODS / DRIVER_DEPS 定义）必须位于所有使用点之前——
# 原来 SEXP_MODS 定义在文件后半部，早于它的 boot-backend-driver 把它
# 读成了空依赖，同样是无人察觉的静默丢失。
SEXP_MODS = lib/sexp.$(HTTP_EXT) lib/sexp_asan.$(HTTP_EXT) lib/sexp_tsan.$(HTTP_EXT) lib/sexp_cov.$(HTTP_EXT)
DRIVER_DEPS = lispvm $(SEXP_MODS)

# backend_driver：`tinyactor run`（lisp 路径）的编译半程驱动——TA 源码经
# lisp 管线（tokenize/parse/lower/compile）出 .bc，由 lispvm 执行（路线图
# 7b-2）。种子 .bc 已入库（7c-1）：fresh clone 直接可用；过期重建 = 用现有
# 种子编自己（固定点 byte-identical）；种子缺失不自动造——报错让
# git checkout 恢复。改 vm-demo/lisp/backend_driver.ta 或其 import 的内核后
# 重跑本目标。file 目标规则见下（必须在 SEXP_MODS 定义之后）。
.PHONY: boot-backend-driver
# $(DRIVER_DEPS)：见上方"Driver 运行期前置"不变量——缺 lib/sexp 时 driver
# 不报错而劣化 abort（TEST_DEPS 串行构建时踩过 Error 134）。
boot-backend-driver: $(DRIVER_DEPS)
	@bc=vm-demo/lisp/boot/backend_driver.bc; \
	test -s $$bc || { echo "错误：种子 $$bc 缺失（入库产物）—— git checkout -- $$bc" >&2; exit 1; }; \
	tmp=$$(mktemp vm-demo/lisp/boot/.backend_driver.XXXXXX) || exit 1; \
	./lispvm -q $$bc vm-demo/lisp/backend_driver.ta "$$tmp" "" && mv -f "$$tmp" $$bc || { rm -f "$$tmp"; exit 1; }; \
	echo "BOOT-DRIVER OK: wrote $$bc"

# lisp 双轨 gate：静态门（编译半程不落 tavm——路线图 7b-2）+ bridge
# （语义表正/负例）+ corpus（test/basic 全量对拍）。--vm=lisp 默认切换的
# 决策数据源；红了就不许切。
.PHONY: lisp-gate
# $(DRIVER_DEPS)：run_bridge/run_corpus 走 tinyactor run（编译半程），
# 裸跑本目标（fresh clone）必须先有 lib/sexp——CI job 里前面的
# `make clean && make` 恰好建过，掩盖了缺失。
lisp-gate: $(DRIVER_DEPS)
	@if grep -nF '"$$TAVM" "$$driver"' tinyactor; then \
		echo "错误：run_lisp 编译半程仍在用 tavm（路线图第 7 步 b-2：应切 lispvm）" >&2; exit 1; \
	fi
	@if grep -nF '"$$BOOTSTRAP" fmt' tinyactor; then \
		echo "错误：tinyactor fmt 仍宿主旧链（路线图第 7 步 b-3：fmt 应切 lispvm）" >&2; exit 1; \
	fi
	@if grep -n "^import codegen" vm-demo/lisp/*.ta; then echo "错误：lisp 链源码不得 import codegen（step7a 已从 lower-ast 拔除，不得回退）"; exit 1; fi
	sh vm-demo/lisp/check_no_codegen_closure.sh
	sh vm-demo/lisp/run_bridge.sh
	sh vm-demo/lisp/run_corpus.sh

HDRS = ta.h ta_inline.h

$(OBJ_DIR)/%.o: src/%.c $(HDRS) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR):
	mkdir -p $@

# E3: demo module — minimal C module template (docs/c-module.md).
# One output per build config (plain / _asan / _tsan / _cov), so a sanitizer
# or coverage build never overwrites the module the plain VM loads.
DEMO_MODS = lib/demo.$(HTTP_EXT) lib/demo_asan.$(HTTP_EXT) lib/demo_tsan.$(HTTP_EXT) lib/demo_cov.$(HTTP_EXT)
$(DEMO_MODS): lib/demo.c $(HDRS)
	$(CC) $(MOD_CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< $(MOD_LDLIBS)

# math / time modules (stdlib-port-plan Workstream A / C) — same lazy-dylib
# pattern as demo. -lm: libm symbols must resolve at dlopen(RTLD_NOW) even
# on Linux where the plain VM link does not export them transitively.
MATH_MODS = lib/math.$(HTTP_EXT) lib/math_asan.$(HTTP_EXT) lib/math_tsan.$(HTTP_EXT) lib/math_cov.$(HTTP_EXT)
$(MATH_MODS): lib/math.c $(HDRS)
	$(CC) $(MOD_CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< -lm $(MOD_LDLIBS)

TIME_MODS = lib/time.$(HTTP_EXT) lib/time_asan.$(HTTP_EXT) lib/time_tsan.$(HTTP_EXT) lib/time_cov.$(HTTP_EXT)
$(TIME_MODS): lib/time.c $(HDRS)
	$(CC) $(MOD_CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< $(MOD_LDLIBS)
# buffer module (stdlib-port-plan Workstream A) — lazy dylib like math/time;
# static registration would make `import buffer` a builtin no-op and
# lib/buffer.ta (the Buffer ADT + lift) would never load.
BUFFER_MODS = lib/buffer.$(HTTP_EXT) lib/buffer_asan.$(HTTP_EXT) lib/buffer_tsan.$(HTTP_EXT) lib/buffer_cov.$(HTTP_EXT)
$(BUFFER_MODS): lib/buffer.c $(HDRS)
	$(CC) $(MOD_CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< $(MOD_LDLIBS)

# process module (stdlib-port-plan Workstream C) — lazy dylib like buffer;
# static registration would make `import process` a builtin no-op and
# lib/process.ta (the Proc ADT + lift) would never load.
PROCESS_MODS = lib/process.$(HTTP_EXT) lib/process_asan.$(HTTP_EXT) lib/process_tsan.$(HTTP_EXT) lib/process_cov.$(HTTP_EXT)
$(PROCESS_MODS): lib/process.c $(HDRS)
	$(CC) $(MOD_CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< $(MOD_LDLIBS)

# sexp module (lispvm bridge, vm-demo/lisp) — lazy dylib like process;
# static registration would make `import sexp` a builtin no-op and
# lib/sexp.ta (the external-fn signatures) would never load.
# 变量 SEXP_MODS 的定义已上移到"Driver 运行期前置"（单一事实来源）：
# 本规则的 prerequisite 读取时展开，必须晚于定义。
$(SEXP_MODS): lib/sexp.c $(HDRS)
	$(CC) $(MOD_CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< $(MOD_LDLIBS)

# .bc file 目标（TEST_DEPS 消费，见上）：种子已入库（7c-1），recipe = 用
# 现有种子自重建（lispvm 编 driver，跑在全闭包 typecheck 上 ~12s）。懒重建
# 放在 per-test 的 run_lisp 里不行：coverage-c（make -j4 + 插桩 VM ~2.5x）
# 下多个冷重建并发挤 2 核，全部超 180s 测试窗口被杀 → 每个测试重复冷重建
# 的死亡螺旋——套件开跑前由 TEST_DEPS 串行建一次（本规则）。
# 运行期需要 $(DRIVER_DEPS)（lispvm + lib/sexp dlopen）：串行构建 TEST_DEPS
# 时若缺依赖，.bc 排在前面会撞 "dlopen failed" → 劣化 abort 134（Error 134）。
vm-demo/lisp/boot/backend_driver.bc: vm-demo/lisp/backend_driver.ta $(DRIVER_DEPS)
	@test -s $@ || { echo "错误：种子 $@ 缺失（入库产物）—— git checkout -- $@" >&2; exit 1; }
	@tmp=$$(mktemp vm-demo/lisp/boot/.backend_driver.XXXXXX) || exit 1; \
	./lispvm -q $@ vm-demo/lisp/backend_driver.ta "$$tmp" "" && mv -f "$$tmp" $@ || { rm -f "$$tmp"; exit 1; }

clean:
	rm -rf $(OBJ) obj_asan obj_tsan obj_cov coverage lispvm lispvm_asan \
		lib/*.so lib/*.dylib

# ============================================================
# Benchmark targets
#   make benchmark           — run all benchmarks
#   make benchmark-regression — run benchmarks and check for regression
#   make benchmark-clean     — clean benchmark results
# ============================================================

# boot-backend-driver 依赖：bench 首跑别在计时里做 driver 重建（16s 的重建
# 会灌进第一轮，把保存的 mean 拉成废数据；还会让 stderr 提示行混进 output）。
benchmark: tinyactor lispvm boot-backend-driver
	@bash benchmark/run_benchmarks.sh

benchmark-regression: tinyactor lispvm boot-backend-driver
	@bash benchmark/run_benchmarks.sh --regression

benchmark-clean:
	rm -rf benchmark/results/*.json benchmark/results/*.csv

# ============================================================
# Test targets
#   make test           — run all test categories
#   make test-basic     — language basics (fast, lightweight)
#   make test-gc        — GC correctness + benchmarks
#   make test-gc-asan   — GC tests with AddressSanitizer
#   make test-gc-tsan   — GC tests with ThreadSanitizer
#   make test-actor     — actor model / concurrency
#   make test-module    — module system
#   make test-compiler  — compiler/parser/typecheck
#   make test-bootstrap — self-hosting + fixed point
#   make test-example   — example scripts
# ============================================================

# $(DRIVER_DEPS) 在列：tinyactor run 默认走 lisp 路径，测试进程需要 lispvm
# 二进制（CI 的 test/coverage/benchmark job 全在这里翻过车——本地手 build
# 过所以绿）；TEST_DEPS 不做 make all，缺 lib/sexp.so 时 driver 编译半程的
# cfunc 解析失败，编译器劣化成无限分配（arena exhausted abort）或符号表
# 缺项（undefined: null?）。
TEST_DEPS = tinyactor $(DRIVER_DEPS) vm-demo/lisp/boot/backend_driver.bc $(DEMO_MODS) $(MATH_MODS) $(TIME_MODS) $(BUFFER_MODS) $(PROCESS_MODS)

test-basic: $(TEST_DEPS)
	@bash test/run_basic_tests.sh

# GC stress runs both VMs: TA_GC_STRESS is a ta.h heap knob, and lispvm uses
# that same heap/GC (issue #249). Measured on the lisp pass: 18/18 pass, slowest
# case gc-pair-churn 100s vs the 300s per-attempt budget — no timeout cliff.
test-gc: $(TEST_DEPS)
	@bash test/run_gc_tests.sh

test-actor: $(TEST_DEPS)
	@bash test/run_actor_tests.sh

test-module: $(TEST_DEPS)
	@bash test/run_module_tests.sh

test-compiler: $(TEST_DEPS)
	@bash test/run_compiler_tests.sh

test-bootstrap: $(TEST_DEPS)
	@bash test/run_bootstrap_tests.sh

test-example: $(TEST_DEPS)
	@bash test/run_example_tests.sh

test-cli: $(TEST_DEPS)
	@bash test/run_cli_tests.sh

test-1m-actor: $(TEST_DEPS)
	@bash test/run_1m_actor.sh

test-gc-long: $(TEST_DEPS)
	@bash test/run_gc_long_tests.sh

# clang-format 版本护栏的正/负例（issue #263）：PATH shim 模拟错版本/对版本，
# 断言 guard 在任何格式化动作之前拦截错版本且不碰工作区。依赖 TEST_DEPS：
# 嵌套 make fmt 里的 ./tinyactor fmt 需要 lispvm + driver.bc（7b-3 fmt 再宿主）
# 与运行期 .so；TEST_DEPS 已全部包含。
test-fmt-guard: $(TEST_DEPS)
	@bash test/run_fmt_guard_tests.sh






test: test-basic test-gc test-actor test-module test-compiler test-example test-cli test-fmt-guard

# ============================================================
# Coverage targets
#   make coverage — one-stop: clean, COV=1 build, run `make test` in
#                   parallel under the instrumented build, merge
#                   profiles, print a report, write .lcov and HARD-FAIL
#                   when line coverage is below COV_MIN%
#   make test-cov — the same build + test run, without the merge/report
#
# The category list is `test`'s own — there is NO second list to keep in
# sync. Coverage differs from a plain `make test` by nothing but COV=1
# plus the two env vars below, so the test verdict and the coverage
# numbers always come from the same execution and can never drift apart.
#
# Requires llvm-profdata / llvm-cov (Homebrew LLVM) in PATH. Every test
# spawns its own VM process; LLVM_PROFILE_FILE uses %p so each process
# writes a separate .profraw that llvm-profdata merges afterwards.
# ============================================================
COV_TOOL     ?= llvm-cov
COV_PROFDATA ?= coverage/coverage.profdata
COV_LCOV     ?= coverage/coverage.lcov
# Hard CI gate: line coverage (LF/LH in the .lcov) must be >= COV_MIN%.
# Ratchet policy: raise this over time as tests improve — the plan is 85+.
# Bump the committed default; to preview a future threshold locally:
#   make coverage COV_MIN=85
COV_MIN      ?= 78
# %p keeps one .profraw per VM process. COV=1 builds exactly one
# instrumented binary — lispvm (via `all`); the lib/*.c modules are compiled
# without instrumentation (see the COV block above), so lispvm is the whole
# coverage universe.
COV_RUN_ENV := LLVM_PROFILE_FILE="$(CURDIR)/coverage/profraw/tavm-%p.profraw"

# C implementation coverage via LLVM instrumentation.
test-cov:
	$(MAKE) clean
	$(COV_RUN_ENV) $(MAKE) COV=1 test

# coverage: run the suite under COV=1, then gate on line coverage.
#
# Exactly one binary carries instrumentation (lispvm — see COV_RUN_ENV
# above), so the lcov export/report is that binary alone: every instrumented
# file it maps (src/*.c, ta.h/ta_inline.h, vm-demo/lisp/lispvm.c) lands in
# $(COV_LCOV) exactly once and the LF/LH sum the gate reads below covers the
# whole instrumented universe. The ignore regex drops build products
# (obj_/) and OpenSSL system headers (third-party, pulled in via src/tls.c).
coverage: test-cov
	@command -v llvm-profdata >/dev/null 2>&1 || { echo "llvm-profdata not found (install Homebrew LLVM; it also provides the clang used for the COV build)" >&2; exit 1; }
	@command -v $(COV_TOOL) >/dev/null 2>&1 || { echo "$(COV_TOOL) not found in PATH" >&2; exit 1; }
	llvm-profdata merge -sparse coverage/profraw/*.profraw -o $(COV_PROFDATA)
	$(COV_TOOL) export lispvm -instr-profile=$(COV_PROFDATA) -format=lcov \
		-ignore-filename-regex='(^|/)obj_/|openssl' > $(COV_LCOV)
	$(COV_TOOL) report lispvm -instr-profile=$(COV_PROFDATA) \
		-ignore-filename-regex='(^|/)obj_/|openssl'
	@line_pct=$$(awk -F: '/^LH:/{lh+=$$2} /^LF:/{lf+=$$2} END { if (lf > 0) printf "%.2f", lh * 100 / lf; else print "0" }' $(COV_LCOV)); \
	gate_fail=$$(awk -v p="$$line_pct" -v min="$(COV_MIN)" 'BEGIN { print (p + 0 < min) ? 1 : 0 }'); \
	echo "LINE COVERAGE: $$line_pct% (gate: >= $(COV_MIN)%)"; \
	if [ "$$gate_fail" = "1" ]; then \
		echo "COVERAGE GATE FAILED: $$line_pct% < $(COV_MIN)% — add tests before merging" >&2; \
		exit 1; \
	fi; \
	echo "COVERAGE GATE PASSED"
	@echo "COVERAGE OK: report above; lcov written to $(COV_LCOV)"

# Sanitizer targets — only for GC tests
test-gc-asan:
	$(MAKE) clean
	$(MAKE) ASAN=1
	bash test/run_gc_tests.sh

test-gc-tsan:
	$(MAKE) clean
	$(MAKE) TSAN=1
	bash test/run_gc_tests.sh

# Legacy full-suite sanitizer targets (run everything under sanitizer).
# The build line must include lispvm: default VM is lisp since #246, clean
# wipes the binary, and without it every runner fails with "lispvm not found".
# The lisp runtime half then runs under ASAN/TSAN too (run_lisp spawns lispvm).
#
# driver prereq（同 TEST_DEPS 的 file 目标）：fresh checkout 下 .bc 缺失，
# run_lisp 懒重建会当场重建种子，CI 2 核 >180s 必被
# per-test 超时杀 → driver 永远建不出 → 每个测试重复冷重建直至 45min job 上限
# （coverage-c 同款死亡螺旋，2026-10-08 sanitizer job 首跑实测）。prereq 在
# recipe 的 clean 之前执行，clean 不删 driver 产物，plain 工具链建的 .bc
# 与 sanitizer 无关（产物字节码相同；编译/运行半程都跑 lispvm——ASAN=1 下
# lispvm 即 asan 构建，sanitizer 覆盖比 7b-2 前更完整）。
test-asan: vm-demo/lisp/boot/backend_driver.bc
	$(MAKE) clean
	$(MAKE) ASAN=1 all
	bash test/run_basic_tests.sh
	bash test/run_gc_tests.sh
	bash test/run_actor_tests.sh
	bash test/run_module_tests.sh
	bash test/run_compiler_tests.sh
	bash test/run_bootstrap_tests.sh
	bash test/run_example_tests.sh

test-tsan: vm-demo/lisp/boot/backend_driver.bc
	$(MAKE) clean
	$(MAKE) TSAN=1 all
	bash test/run_basic_tests.sh
	bash test/run_gc_tests.sh
	bash test/run_actor_tests.sh
	# json-mpc takes 169s under TSAN (vs 20s plain); 60s budget made it the
	# only flake in the suite — widen the per-attempt cap for this runner.
	TEST_TIMEOUT=300 bash test/run_module_tests.sh
	bash test/run_compiler_tests.sh
	bash test/run_bootstrap_tests.sh
	bash test/run_example_tests.sh

# ============================================================
# kernfuzz fast ring (docs/kernel-fuzzing-design.md §9, DELIV-9)
#
#   make kernfuzz-fast — push-triggered quick ring, three sub-rings:
#     1. morph differential: 300 frozen-seed baseline + 200 rolling
#        seeds (M-7) = 500 base programs ×4 exec units, per-program
#        timeout 2s (R3 C-4)
#     2. fmt idempotence scan over the same corpus (tc_meta.check_fmt)
#     3. typecheck frozen-negative replay (test/kernfuzz-frozen/)
#
#   Budget (R3 C-4), first-day measured (macOS arm64, 2026-09-03):
#     build 51 ms / run 65 ms / 500×4×(51+65) = 232 s (naive projection)
#     full 500-program composition actual wall: 617 s (≈10.3 min, > 5 min
#     budget) — so per the §9 "若超 5min 按比例缩减 fast 规模" clause the
#     ring ships with KERNFUZZ_FAST_SCALE=0.4 (200 programs, measured
#     220 s ≈ 3.7 min).  See the budget table in
#     docs/kernel-fuzzing-design.md §9.  Set KERNFUZZ_FAST_SCALE=1.0 for
#     the full composition.
#
#   Exit semantics (§9): toolchain missing → "KERNFUZZ-SKIPPED" + exit 0;
#   any finding → exit 1.
#
#   Rolling-seed reproducibility check (same day+commit+counter → same
#   sequence):
#     python3 tools/kernfuzz/fast.py rolling-seeds \
#         --date $(shell date +%F) --counter 0 --count 5
# ============================================================

# lispvm_asan: the recursive ASAN=1 invocation (obj_asan/ is separate from the
# plain obj dir, so the two builds coexist).  Built UNCONDITIONALLY before
# every kernfuzz run — make's dependency scan is the staleness gate, so a VM
# source change rebuilds it and an up-to-date binary costs a no-op scan.
# (The old 'only when missing' policy let a stale ASan VM run new-format
# bytecode: nightly 2026-09-29 produced 1082 phantom findings before the
# silent exit-0 / garbage-message mismatch was traced to the binary lag.)
# It is also the morph lisp arm's ASan base (lisparm.py), same staleness
# rationale, same separate-output-name rule. (7c-3: the non-lisp morph arms
# still name ./tavm_asan — a dead reference until the kernfuzz re-anchor.)
# The driver file target is the compile half's staleness gate.
# lib/sexp_asan.* joins the line because the driver dlopens the sexp
# module at startup — missing → the cfunc-resolution degradation of
# TEST_DEPS' note (lib/sexp.ta), i.e. silently miscompiled findings.
kernfuzz-fast: tinyactor lispvm vm-demo/lisp/boot/backend_driver.bc
	@$(MAKE) --no-print-directory ASAN=1 lispvm_asan lib/sexp_asan.$(HTTP_EXT) || exit 1;
		KERNFUZZ_PROGRESS=1 KERNFUZZ_FAST_SCALE=$${KERNFUZZ_FAST_SCALE:-0.4} python3 -u tools/kernfuzz/fast.py

# Regenerate the frozen tc-negative snapshot from the fixed seed list
# (commit the result; fast ring only replays it).
kernfuzz-freeze-tc: tinyactor lispvm vm-demo/lisp/boot/backend_driver.bc
	@$(MAKE) --no-print-directory ASAN=1 lispvm_asan lib/sexp_asan.$(HTTP_EXT) || exit 1;
	python3 tools/kernfuzz/fast.py freeze-tc

# Verify regenerated frozen AST snapshots match the committed corpus.
# $(DRIVER_DEPS)：gate = `tinyactor run` ×83（编译半程 dlopen lib/sexp），
# fresh clone 直跑本目标必须先建出运行期模块——#275 把 $(TARGET) 换成
# lispvm 时丢了这条传递依赖，coverage-c 83 语料全撞 arena abort。
kernfuzz-snapshot-check: tinyactor $(DRIVER_DEPS)
	@guile tools/kernfuzz/snapshot.scm || exit 1
	@git diff --exit-code -- test/kernfuzz-frozen/ || { \
		echo "语料源码变更需同步再生成冻结快照" >&2; exit 1; \
	}

# ============================================================
# kernfuzz nightly ring (docs/kernel-fuzzing-design.md §9, DELIV-10)
#
#   make kernfuzz-nightly — nightly-triggered slow ring, six components
#   (composition only; every component is an existing kernfuzz module):
#     1. golden frozen-corpus full pass (snapshot drift + DELIV-2 anchor)
#     2. morph differential: 1000 rolling seeds (M-7), Tier A+B transforms
#     3. typecheck bidirectional: 2000 cases generated fresh (not frozen)
#     4. GC sequential diff (gc_seqdiff, 100-seed window, N=1)
#     5. message multiset harness (multiset, K/M matrix parameterized)
#     6. TSan long run (W-chaos, §7.4 30min cap) — M-13: on macOS a
#        failed/unavailable TSAN build is recorded as an explicit SKIP
#        observation (build/kernfuzz/nightly/tsan/tsan-skip.json), never
#        a silent pass; the TSan frontline targets Linux x86_64 CI.
#
#   Exit semantics (§9, opposite of fast): toolchain missing → exit 1
#   (the slow ring is NOT allowed to skip silently); any novel finding →
#   exit 1; all green → exit 0 and the nightly rolling counter advances.
#
#   Rolling seeds use nightly's OWN counter file
#   (build/kernfuzz/rolling-counter-nightly — independent of fast's).
#   Same commit + date + counter → same seed block (M-7 reproducible).
#
#   Scale down for a local dry run:
#     make kernfuzz-nightly KERNFUZZ_NIGHTLY_SCALE=0.05
#   Tier C (CPS) stays excluded: --with-cps exists but is refused (exit 1)
#   until the §5.2 corpus gate passes.
# ============================================================

kernfuzz-nightly: tinyactor lispvm vm-demo/lisp/boot/backend_driver.bc
	@$(MAKE) --no-print-directory ASAN=1 lispvm_asan lib/sexp_asan.$(HTTP_EXT) || exit 1;
	KERNFUZZ_NIGHTLY_SCALE=$${KERNFUZZ_NIGHTLY_SCALE:-1.0} python3 tools/kernfuzz/nightly.py

# Bootstrap（7c-1 换锚）：旧链「tavm + bootstrap.tabc 重编 build.ta」随
# codegen/.tabc 世界删除；新自举锚 = 入库的 backend_driver.bc 种子。
# 本目标 = 用现有种子自重建 driver 全闭包并原地写回：改过 lib/bootstrap/*.ta
# 或 vm-demo/lisp/*.ta 源码后跑它，产出新种子提交入库；固定点 gate 由
# `make test-bootstrap` 把重建产物与入库种子逐字节比对（连跑两遍产物
# 一致 = fixed point，AGENTS 约定的语义原样保留）。
#
# A3 pipeline-safety: a pipe (`make bootstrap 2>&1 | tail -1`) reports the
# LAST command's status (tail → 0), masking a real failure. The recipe never
# trusts a pipeline exit code; it verifies the real compile result internally
# (artifact non-empty before mv) and prints an unambiguous verdict as its
# LAST line, so a piped `tail -1` still shows the truth. Callers that need a
# guaranteed-correct status must use `set -o pipefail` (GitHub Actions does
# by default) or PIPESTATUS.
bootstrap: $(DRIVER_DEPS)
	@bc=vm-demo/lisp/boot/backend_driver.bc; \
	test -s $$bc || { echo "BOOTSTRAP FAILED: seed $$bc missing (入库产物) — git checkout -- $$bc" >&2; exit 1; }; \
	rm -f vm-demo/lisp/boot/.backend_driver.rebuild; \
	./lispvm -q $$bc vm-demo/lisp/backend_driver.ta vm-demo/lisp/boot/.backend_driver.rebuild "" || { rm -f vm-demo/lisp/boot/.backend_driver.rebuild; echo "BOOTSTRAP FAILED: self-rebuild errored" >&2; exit 1; }; \
	test -s vm-demo/lisp/boot/.backend_driver.rebuild || { echo "BOOTSTRAP FAILED: self-rebuild produced no artifact" >&2; exit 1; }; \
	mv -f vm-demo/lisp/boot/.backend_driver.rebuild $$bc; \
	echo "BOOTSTRAP OK: wrote $$bc"

# ============================================================
# Formatting targets
#   make fmt       — format all C/C++ (clang-format) and lib/*.ta (tinyactor)
#   make fmt-check — verify both are properly formatted (exit 1 if not)
#
# 版本护栏（issue #263）：CI 的 fmt-check 只在 macOS job 跑，钉 brew
# llvm@18（18.1.8）。PATH 里另一个 major 版本跑一次 make fmt 就会把
# lispvm.c 宏续行 \ 的列对齐重写成纯空白 churn。因此 fmt/fmt-check
# 都前置依赖 fmt-version-check：版本不符在任何格式化动作之前直接失败。
# ============================================================
CLANG_FORMAT_MAJOR := 18

.PHONY: fmt-version-check
fmt-version-check:
	@if ! command -v clang-format >/dev/null 2>&1; then \
		echo "ERROR: clang-format not found (fmt version guard expects $(CLANG_FORMAT_MAJOR).x)" >&2; \
		echo 'install: brew install llvm@18 && PATH="$$(brew --prefix llvm@18)/bin:$$PATH" make fmt' >&2; \
		exit 1; \
	fi; \
	actual="$$(clang-format --version 2>&1)"; \
	major="$$(printf '%s\n' "$$actual" | grep -oE '[0-9]+\.[0-9]+' | head -n 1 | cut -d. -f1)"; \
	if [ "$$major" != "$(CLANG_FORMAT_MAJOR)" ]; then \
		echo "ERROR: clang-format version mismatch — fmt guard refuses to format" >&2; \
		echo "  expected: $(CLANG_FORMAT_MAJOR).x (CI fmt-check: brew llvm@18 = 18.1.8)" >&2; \
		echo "  actual:   $$actual" >&2; \
		echo 'install: brew install llvm@18 && PATH="$$(brew --prefix llvm@18)/bin:$$PATH" make fmt' >&2; \
		exit 1; \
	fi

fmt: fmt-version-check tinyactor lispvm vm-demo/lisp/boot/backend_driver.bc
	@find . -path "./.tinyactor-build*" -prune -o -type f \( -name "*.c" -o -name "*.h" -o -name "*.cpp" -o -name "*.hpp" \) \
		-not -path "./.git/*" -not -path "./.vscode/*" \
		-exec clang-format -i {} \;
	@for f in lib/*.ta lib/bootstrap/*.ta; do ./tinyactor fmt "$$f"; done
	@echo "C/C++ and lib/*.ta formatted"

fmt-check: fmt-version-check tinyactor lispvm vm-demo/lisp/boot/backend_driver.bc
	@echo "Checking code formatting..."
	@out="$$(find . -path "./.tinyactor-build*" -prune -o -type f \( -name "*.c" -o -name "*.h" -o -name "*.cpp" -o -name "*.hpp" \) \
		-not -path "./.git/*" -not -path "./.vscode/*" \
		-exec clang-format --dry-run --Werror {} \; 2>&1)"; \
	if [ -n "$$out" ]; then \
		echo "$$out"; \
		echo "FORMAT VIOLATIONS FOUND (run 'make fmt')" >&2; \
		exit 1; \
	fi
	@for f in lib/*.ta lib/bootstrap/*.ta; do \
		if ! ./tinyactor fmt --check "$$f" >/dev/null; then \
			echo "fmt-check FAILED: $$f (run 'make fmt')" >&2; \
			exit 1; \
		fi; \
	done
