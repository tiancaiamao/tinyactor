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
#   ASAN=1 make tavm  → build with AddressSanitizer
#   TSAN=1 make tavm  → build with ThreadSanitizer
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
# tavm (obj_cov) that loads its own lib/demo_cov module and never
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
# MOD_SUFFIX is the single source of truth for the loader's per-config module
# filename infix (main.c formats lib/<mod>$(MOD_SUFFIX).<ext> under TA_MOD_TAG).
# It is exported so the test harness names any stub modules it fabricates the
# same way without re-deriving the tag logic in shell.
MOD_SUFFIX := $(COV_TAG:%=_%)$(SAN:%=_%)

DEMO_LIB := lib/demo$(MOD_SUFFIX).$(HTTP_EXT)
MATH_LIB := lib/math$(MOD_SUFFIX).$(HTTP_EXT)
TIME_LIB := lib/time$(MOD_SUFFIX).$(HTTP_EXT)
BUFFER_LIB := lib/buffer$(MOD_SUFFIX).$(HTTP_EXT)
PROCESS_LIB := lib/process$(MOD_SUFFIX).$(HTTP_EXT)
SEXP_LIB := lib/sexp$(MOD_SUFFIX).$(HTTP_EXT)
ENCODING_LIB := lib/encoding$(MOD_SUFFIX).$(HTTP_EXT)
RANDOM_LIB := lib/random$(MOD_SUFFIX).$(HTTP_EXT)
STR_LIB := lib/str$(MOD_SUFFIX).$(HTTP_EXT)
TLS_LIB := lib/tls$(MOD_SUFFIX).$(HTTP_EXT)
FILE_LIB := lib/file$(MOD_SUFFIX).$(HTTP_EXT)
OS_LIB := lib/os$(MOD_SUFFIX).$(HTTP_EXT)
NET_LIB := lib/net$(MOD_SUFFIX).$(HTTP_EXT)
TIMER_LIB := lib/timer$(MOD_SUFFIX).$(HTTP_EXT)
COV_LIB := lib/cov$(MOD_SUFFIX).$(HTTP_EXT)

# Export the per-config module infix to every recipe's environment so test
# harnesses that fabricate stub modules (test/run_crash_tests.sh) name them the
# way the loader looks them up — no tag logic re-derived in shell.
export MOD_SUFFIX

ifdef GC_DEBUG
  CFLAGS += -DGC_DEBUG=1
endif

# ============================================================
# OpenSSL for the tls module (stdlib-port-plan Phase 6 #1) — consumed by
# the TLS_MODS rule ONLY. lib/tls.c is a lazy dylib (Phase A5-a): OpenSSL
# is a link dependency of lib/tls*.dylib and never of the tavm binary, so
# a missing brew openssl@3 breaks the tls module build, not the whole VM.
#   macOS: Homebrew openssl@3 (keg-only, not on the default include/lib
#          search path) — fall back to pkg-config openssl when brew (or
#          the formula) is absent.
#   Linux: system libssl-dev via pkg-config, or plain -lssl -lcrypto.
# Per sanitizer variant: the TLS_MODS recipe adds TLS_CFLAGS/TLS_LIBS on
# top of MOD_CFLAGS/MOD_LDLIBS, so each variant links OpenSSL itself.
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


# Linux needs -ldl for dlopen/dlsym; macOS has it in libSystem
ifneq ($(UNAME_S),Darwin)
LDLIBS += -ldl
LDLIBS += -lm
# glibc hides POSIX declarations under strict -std=c99 (__STRICT_ANSI__),
# e.g. gethostname in unistd.h — gcc's gnu defaults masked this until the
# clang-based COV build hit it on Ubuntu CI. The VM uses POSIX APIs
# throughout (net/file/os/tls), so request them explicitly. No-op on BSD.
CFLAGS += -D_DEFAULT_SOURCE
# Same request for the module builds. plain / ASAN 的 MOD_CFLAGS 直接继承
# CFLAGS（上一条已在里面，对它们是重复但无害的一条 -D），COV 的 MOD_CFLAGS
# 却是不继承 CFLAGS 的手写清单（见上面 COV 块）——漏掉这条，lib/*.c 在
# Ubuntu coverage job 上又退回 strict c99：lib/os.c 的 gethostname 正是这样
# 第二次红的（第一次是 VM 里的 src/os.c，即上一条的由来）。两处保持同步。
MOD_CFLAGS += -D_DEFAULT_SOURCE
endif
# Export symbols for dynamically loaded modules (needed on Linux for dlopen)
ifeq ($(UNAME_S),Linux)
RDYNAMIC = -Wl,--export-dynamic
else
# macOS: allow shared lib to have unresolved symbols resolved at load time
UNDEF_OK = -undefined dynamic_lookup
endif

SRC     = src/val.c src/vm.c src/builtin.c src/scheduler.c src/gc.c src/api.c src/num.c src/prof.c src/main.c
OBJ     = $(SRC:src/%.c=$(OBJ_DIR)/%.o)

.PHONY: all clean test test-basic test-gc test-actor test-module test-compiler \
        test-bootstrap test-example test-cli test-gc-asan test-gc-tsan \
        test-asan test-tsan test-cov coverage test-1m-actor test-gc-long \
        test-unit-gate test-kl-link test-kl-sem \
        bootstrap benchmark benchmark-regression \
        benchmark-clean fmt fmt-version-check kernfuzz-fast kernfuzz-freeze-tc \
        test-fmt-guard \
        kernfuzz-nightly kernfuzz-snapshot-check

# Default build ships the complete C-module set: a bare `make clean; make`
# must leave a runtime where scripts can actually call demo/math/time/
# buffer/process (the dylibs are lazy-loaded; a missing one makes the call
# silently push nil instead of erroring).
all: tavm $(DEMO_LIB) $(MATH_LIB) $(TIME_LIB) $(BUFFER_LIB) $(PROCESS_LIB) $(SEXP_LIB) $(ENCODING_LIB) $(RANDOM_LIB) $(STR_LIB) $(TLS_LIB) $(FILE_LIB) $(OS_LIB) $(NET_LIB) $(TIMER_LIB) $(COV_LIB)

# tavm: the VM binary — loader + main in src/main.c，随 SRC 进
# $(OBJ)（D3：与运行时对象同一编译单元集合），链接 TA 运行时（values、
# heap、GC、scheduler、C modules）。
.PHONY: tavm
# 原子重链（-o tmp && mv）：tavm 是 .PHONY，`make fmt` 等任何嵌套 make 都会
# 触发重链。GNU ld 的 -o 会先在目标路径建 0644 文件、链接完才 chmod——窗口内
# 并行 make -j8 test 的测试进程 exec 它得到 "Permission denied" (exit 126)
# （PR #274 ubuntu 首跑：fmt-guard 嵌套 make fmt × 基本类测试并发踩中）。mktemp
# 先建后 rm 再给 cc：mktemp 文件是 0600，留着会让产物永久不可执行。
tavm: $(OBJ)
	@tmp=$$(mktemp tavm.XXXXXX) || exit 1; rm -f "$$tmp"; \
	$(CC) $(CFLAGS) $(RDYNAMIC) -o "$$tmp" $(OBJ) -lpthread $(LDLIBS) || { rm -f "$$tmp"; exit 1; }; \
	rm -rf "$$tmp.dSYM"; mv -f "$$tmp" $@

# tavm_asan：kernfuzz morph tavm 臂（tools/kernfuzz/tavm_arm.py）的 ASan
# 底座。独立输出名——test-asan 故意用 ASAN=1 make tavm 覆盖 plain 版，
# kernfuzz 不得搅动 ./tavm（一条分支一个问题，不顺手动它）。
# 仅 SAN=asan 配置下有此规则：裸 `make tavm_asan` 无规则报错响亮，
# 不会静默建出无插桩的赝品。
ifeq ($(SAN),asan)
tavm_asan: $(OBJ)
	$(CC) $(CFLAGS) $(RDYNAMIC) -o $@ $(OBJ) -lpthread $(LDLIBS)
endif

# tavm_tsan：与 tavm_asan 同模式的 TSan 底座（nightly ring 6 W-chaos 用，
# tools/kernfuzz/nightly.py 探测 `TSAN=1 tavm_tsan`）。同理仅 SAN=tsan
# 配置下有此规则、独立输出名不搅动 ./tavm。
ifeq ($(SAN),tsan)
tavm_tsan: $(OBJ)
	$(CC) $(CFLAGS) $(RDYNAMIC) -o $@ $(OBJ) -lpthread $(LDLIBS)
endif

# ============================================================
# Driver 运行期前置 —— 单一事实来源（invariant）
#
# INVARIANT：任何会在 recipe 里执行 driver 编译半程的 make 目标——
#   `./tavm -q lib/bootstrap.tabc ...`（bootstrap /
#   .tabc file 目标）或 `./tinyactor run|build|fmt`
#   （test-* / kernfuzz-* / backend-gate / benchmark / fmt*）——
# prerequisite 必须带上 $(DRIVER_DEPS)：直接引用本变量（TEST_DEPS /
# backend-gate / bootstrap / kernfuzz-snapshot-check），
# 或传递依赖携带它的目标——.tabc 文件目标（其 prerequisite 含
# $(DRIVER_DEPS)，见下）——跑 driver 必有
# .tabc，这条传递依赖总是成立（test-asan / kernfuzz-fast / fmt /
# benchmark 等即此类）。新增这类目标时引用本变量或上述目标，不要再逐处
# 粘贴 tavm + $(SEXP_MODS)（人肉记得加已被证明不可靠）。
#
# 丢了它的代价以前不是响亮报错，而是静默劣化：driver 运行期按模块名 dlopen
# lib/sexp.$EXT（main.c find_cfunc_autoload；lib/sexp.c = S-expr reader，
# cfunc/源码解析用）。模块缺失时 driver 不报错，改走"无限分配"路径一路
# 吃到 actor arena 上限，报出来的是误导性的：
#   tavm: fatal: actor heap arena exhausted (stack outgrew the arena)
# 现在（cfunc 解析不到即 fatal，main.c find_cfunc_autoload）同一场景是两行
# 直接的诊断——
#   tavm: cannot load C module 'sexp' from lib/sexp.dylib: <dlerror>
#   tavm: build the module first (e.g. `make lib/sexp.dylib`) or check the module name
# ——报错只是让"忘了加依赖"一眼可见；依赖本身照样必须有，不是可以不修。
# 本地手 build 过就绿、fresh clone 才红——coverage-c 自 #275 起 83 语料
# 全红即此因（kernfuzz-snapshot-check 把 $(TARGET) 换成 tavm 时连带
# 丢了这条传递依赖）。
#
# 展开时机坑：GNU make 对显式规则的 prerequisite 是读取时立即展开的，
# 所以本块（SEXP_MODS / DRIVER_DEPS 定义）必须位于所有使用点之前——
# 原来 SEXP_MODS 定义在文件后半部，早于本块的 driver 重建目标把它
# 读成了空依赖，同样是无人察觉的静默丢失。
# str module — the compiler itself calls str.* at runtime (tokenizer /
# typecheck / compile are all built on it), so the str dylibs are a driver
# runtime prerequisite exactly like lib/sexp. Defined here (before
# DRIVER_DEPS reads it — same expansion-order note as SEXP_MODS) so every
# driver-running target builds them, not just the test suite.
STR_MODS = lib/str.$(HTTP_EXT) lib/str_asan.$(HTTP_EXT) lib/str_tsan.$(HTTP_EXT) lib/str_cov.$(HTTP_EXT)
SEXP_MODS = lib/sexp.$(HTTP_EXT) lib/sexp_asan.$(HTTP_EXT) lib/sexp_tsan.$(HTTP_EXT) lib/sexp_cov.$(HTTP_EXT)
# file / os modules — driver.runtime prerequisites for the same reason as str:
# the compiler (driver.ta/modsig.ta) imports file, and driver.ta calls
# os.exit, so the dylibs must exist before the first compile half runs.
# Same expansion-order note as STR_MODS/SEXP_MODS.
FILE_MODS = lib/file.$(HTTP_EXT) lib/file_asan.$(HTTP_EXT) lib/file_tsan.$(HTTP_EXT) lib/file_cov.$(HTTP_EXT)
OS_MODS = lib/os.$(HTTP_EXT) lib/os_asan.$(HTTP_EXT) lib/os_tsan.$(HTTP_EXT) lib/os_cov.$(HTTP_EXT)
# net 不进 DRIVER_DEPS：编译半程（driver.ta）无 net.* 引用；但必须进
# TEST_DEPS——测试程序运行期的首次 net.* 调用走懒加载，COV 等变体构建
# 只建 TEST_DEPS 闭包，缺了就是 CI 上 "cannot load C module 'net'"
NET_MODS = lib/net.$(HTTP_EXT) lib/net_asan.$(HTTP_EXT) lib/net_tsan.$(HTTP_EXT) lib/net_cov.$(HTTP_EXT)
TIMER_MODS = lib/timer.$(HTTP_EXT) lib/timer_asan.$(HTTP_EXT) lib/timer_tsan.$(HTTP_EXT) lib/timer_cov.$(HTTP_EXT)
COV_MODS = lib/cov.$(HTTP_EXT) lib/cov_asan.$(HTTP_EXT) lib/cov_tsan.$(HTTP_EXT) lib/cov_cov.$(HTTP_EXT)
# buffer module — the .tabc writer (lib/bootstrap/bytecode.ta) composes its
# text in a Buffer, so the dylib is a driver runtime prerequisite exactly
# like str/file/os. Defined here, before DRIVER_DEPS reads it (same
# expansion-order note as STR_MODS); only the rule lives further down.
BUFFER_MODS = lib/buffer.$(HTTP_EXT) lib/buffer_asan.$(HTTP_EXT) lib/buffer_tsan.$(HTTP_EXT) lib/buffer_cov.$(HTTP_EXT)
DRIVER_DEPS = tavm $(SEXP_MODS) $(STR_MODS) $(FILE_MODS) $(OS_MODS) $(BUFFER_MODS)

# 后端双轨 gate：静态门（编译半程必须走种子宿主，不得旧链直连）+ bridge
# （语义表正/负例）+ corpus（test/basic 全量对拍）。默认 VM 切换的
# 决策数据源；红了就不许切。
.PHONY: backend-gate
# $(DRIVER_DEPS)：run_bridge/run_corpus 走 tinyactor run（编译半程），
# 裸跑本目标（fresh clone）必须先有 lib/sexp——CI job 里前面的
# `make clean && make` 恰好建过，掩盖了缺失。
backend-gate: $(DRIVER_DEPS)
	@if grep -nF '"$$TAVM" "$$driver"' tinyactor; then \
		echo "错误：run_tavm 编译半程仍是旧链直连（应走 -q 种子宿主）" >&2; exit 1; \
	fi
	@if grep -nF '"$$BOOTSTRAP" fmt' tinyactor; then \
		echo "错误：tinyactor fmt 仍宿主旧链（应走 -q 种子宿主）" >&2; exit 1; \
	fi
	@if grep -n "^import codegen" test/backend/*.ta; then echo "错误：backend 链源码不得 import codegen（step7a 已从 lower-ast 拔除，不得回退）"; exit 1; fi
	sh test/backend/check_no_codegen_closure.sh
	sh test/backend/run_bridge.sh
	sh test/backend/run_corpus.sh

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
# Variable BUFFER_MODS is defined above (with DRIVER_DEPS); only the rule
# lives here with the other lazy-dylib module rules.
$(BUFFER_MODS): lib/buffer.c $(HDRS)
	$(CC) $(MOD_CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< $(MOD_LDLIBS)

# encoding module — lazy dylib like buffer; the raw_* primitives moved out of
# src/encoding.c. Statically registering them would make `import encoding` a
# builtin no-op and lib/encoding.ta (the Result lift) would never load.
ENCODING_MODS = lib/encoding.$(HTTP_EXT) lib/encoding_asan.$(HTTP_EXT) lib/encoding_tsan.$(HTTP_EXT) lib/encoding_cov.$(HTTP_EXT)
$(ENCODING_MODS): lib/encoding.c $(HDRS)
	$(CC) $(MOD_CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< $(MOD_LDLIBS)

# random module — lazy dylib like buffer/encoding; the raw_* primitives moved
# out of src/random.c. Statically registering them would make `import random`
# a builtin no-op and lib/random.ta (the Result/Option lift) would never load.
RANDOM_MODS = lib/random.$(HTTP_EXT) lib/random_asan.$(HTTP_EXT) lib/random_tsan.$(HTTP_EXT) lib/random_cov.$(HTTP_EXT)
$(RANDOM_MODS): lib/random.c $(HDRS)
	$(CC) $(MOD_CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< $(MOD_LDLIBS)

# str module — lazy dylib like encoding/random; the primitives moved out of
# src/str.c. Statically registering them would make `import str` a builtin
# no-op and lib/str.ta (the external-fn signature layer) would never load.
# Variable STR_MODS is defined above (with DRIVER_DEPS); only the rule lives
# here with the other lazy-dylib module rules.
$(STR_MODS): lib/str.c $(HDRS)
	$(CC) $(MOD_CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< $(MOD_LDLIBS)

# tls module — lazy dylib like str/encoding; the primitives moved out of
# src/tls.c (Phase A5-a). Statically registering them would make `import
# tls` a builtin no-op and lib/tls.ta (the facade) would never load.
# The ONLY module with an external link dependency: OpenSSL goes in this
# rule (TLS_CFLAGS/TLS_LIBS), so the tavm binary itself never links it.
TLS_MODS = lib/tls.$(HTTP_EXT) lib/tls_asan.$(HTTP_EXT) lib/tls_tsan.$(HTTP_EXT) lib/tls_cov.$(HTTP_EXT)
$(TLS_MODS): lib/tls.c $(HDRS)
	$(CC) $(MOD_CFLAGS) $(TLS_CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< $(MOD_LDLIBS) $(TLS_LIBS)

# file module — lazy dylib like str; the 9 primitives moved out of
# src/file.c. Statically registering them would make `import file` a
# builtin no-op and lib/file.ta (the external-fn signature layer for the
# 6 extended fs fns) would never load.
$(FILE_MODS): lib/file.c $(HDRS)
	$(CC) $(MOD_CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< $(MOD_LDLIBS)

# os module — lazy dylib like buffer; the 4 raw_* primitives moved out of
# src/os.c. They register FLAT (os.raw_* names, no vm_register_module entry)
# so is_builtin_module("os") stays false and `import os` remains a real
# import loading lib/os.ta (the Option/Result lift + pub fns incl. exit).
$(OS_MODS): lib/os.c $(HDRS)
	$(CC) $(MOD_CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< $(MOD_LDLIBS)

# net module — lazy dylib like file; the 7 primitives moved out of
# src/net.c (the last module-level .c in src/). Keeps the net.* names
# (vm_register_module("net") in vm_load_self): tcp.ta's external_fn
# signatures and the bootstrap typecheck promises both land there.
# `import net` 是 host-module no-op（api.c is_builtin_module 硬编码），
# 运行期首次 net.* 引用走懒加载。
$(NET_MODS): lib/net.c $(HDRS)
	$(CC) $(MOD_CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< $(MOD_LDLIBS)

# timer module — lazy dylib like net; the 3 primitives + the poller hooks
# moved out of src/timer.c. vm_load_self registers timer.* AND installs
# VM.timer_deadline_hook/timer_fire_hook (the only core->module reverse
# edge: the io poller's deadline scan, NULL = timer unused).
$(TIMER_MODS): lib/timer.c $(HDRS)
	$(CC) $(MOD_CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< $(MOD_LDLIBS)

# cov module — lazy dylib like net, no reverse edge at all (the poller
# never touches it); cov_dump_env died with the move (no caller).
$(COV_MODS): lib/cov.c $(HDRS)
	$(CC) $(MOD_CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< $(MOD_LDLIBS)

# process module (stdlib-port-plan Workstream C) — lazy dylib like buffer;
# static registration would make `import process` a builtin no-op and
# lib/process.ta (the Proc ADT + lift) would never load.
PROCESS_MODS = lib/process.$(HTTP_EXT) lib/process_asan.$(HTTP_EXT) lib/process_tsan.$(HTTP_EXT) lib/process_cov.$(HTTP_EXT)
$(PROCESS_MODS): lib/process.c $(HDRS)
	$(CC) $(MOD_CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< $(MOD_LDLIBS)

# sexp module (tavm bridge, test/backend) — lazy dylib like process;
# static registration would make `import sexp` a builtin no-op and
# lib/sexp.ta (the external-fn signatures) would never load.
# 变量 SEXP_MODS 的定义已上移到"Driver 运行期前置"（单一事实来源）：
# 本规则的 prerequisite 读取时展开，必须晚于定义。
$(SEXP_MODS): lib/sexp.c $(HDRS)
	$(CC) $(MOD_CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< $(MOD_LDLIBS)

# .tabc file 目标（TEST_DEPS 消费，见上）：种子已入库（7c-1），recipe = 用
# 现有种子自重建（tavm 编 driver，跑在全闭包 typecheck 上 ~12s）。懒重建
# 放在 per-test 的 run_tavm 里不行：coverage-c（make -j4 + 插桩 VM ~2.5x）
# 下多个冷重建并发挤 2 核，全部超 180s 测试窗口被杀 → 每个测试重复冷重建
# 的死亡螺旋——套件开跑前由 TEST_DEPS 串行建一次（本规则）。
# 运行期需要 $(DRIVER_DEPS)（tavm + lib/sexp dlopen）：串行构建 TEST_DEPS
# 时若缺依赖，.tabc 排在前面会撞 "dlopen failed" → 劣化 abort 134（Error 134）。
# D10 合一：重建实现的唯一来源是 `bootstrap` 目标（固定名 + 空产物校验 +
# 末行明确判决）——原 mktemp 路径失败会把临时产物留在工作区，
# 已随目标合一一并消灭；本规则只在种子过期时调它一次。
lib/bootstrap.tabc: lib/bootstrap/driver.ta $(DRIVER_DEPS)
	@$(MAKE) --no-print-directory bootstrap

# .kl 快照与种子是同一次 `make bootstrap` 的两个产物：它本地写出（不入 git），
# 供 G-C 测试与人工观察。谁过期都指向同一个 recipe，避免两条重建路径漂移。
lib/bootstrap/driver.kl: lib/bootstrap/driver.ta $(DRIVER_DEPS)
	@$(MAKE) --no-print-directory bootstrap

clean:
	rm -rf $(OBJ) obj_asan obj_tsan obj_cov coverage tavm tavm_asan \
		tavm_tsan lib/*.so lib/*.dylib lib/bootstrap/driver.kl

# ============================================================
# Benchmark targets
#   make benchmark            — run all runtime benchmarks
#   make benchmark-regression — run benchmarks and check for regression
#   make benchmark-compile    — compile-time bench (median-of-11)
#   make benchmark-vs-go      — TA vs Go: HTTP service throughput + latency
#   make benchmark-clean      — clean benchmark results
# ============================================================

# bootstrap 依赖：bench 首跑别在计时里做 driver 重建（16s 的重建
# 会灌进第一轮，把保存的 mean 拉成废数据；还会让 stderr 提示行混进 output）。
benchmark: tinyactor tavm bootstrap
	@bash benchmark/run_benchmarks.sh

benchmark-regression: tinyactor tavm bootstrap
	@bash benchmark/run_benchmarks.sh --regression

# benchmark-compile：编译时间基准（benchmark/compile/，issue #258）。
# 依赖 tinyactor/tavm/bootstrap 同 benchmark：被测对象要在计时之外先建好。
# MODE 透传给 run.sh：cold（默认）| warm | ratio，例如
#   make benchmark-compile MODE=ratio
benchmark-compile: tinyactor tavm bootstrap
	@bash benchmark/compile/run.sh $(MODE)

# benchmark-vs-go：TA vs Go 端到端对比（benchmark/vs-go/）——同一个压测
# 客户端打两个服务（server.ta vs server.go），比吞吐 / 延迟分布。
# 需要 go 工具链；退出码 1 表示环境缺失（go / tinyactor 未就绪）。
benchmark-vs-go: tinyactor tavm lib/bootstrap.tabc
	@bash benchmark/vs-go/run.sh

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

# $(DRIVER_DEPS) 在列：tinyactor run 默认走 tavm 路径，测试进程需要 tavm
# 二进制（CI 的 test/coverage/benchmark job 全在这里翻过车——本地手 build
# 过所以绿）；TEST_DEPS 不做 make all，缺 lib/sexp.so 时 driver 编译半程
# 的第一个 sexp.* 调用就 fatal（`cannot load C module 'sexp' from
# lib/sexp.so: <dlerror>`，见 main.c find_cfunc_autoload）——以前是静默压 nil，劣化成无限分配（arena exhausted abort）
# 或符号表缺项（undefined: null?）。
TEST_DEPS = tinyactor $(DRIVER_DEPS) lib/bootstrap.tabc $(DEMO_MODS) $(MATH_MODS) $(TIME_MODS) $(BUFFER_MODS) $(PROCESS_MODS) $(ENCODING_MODS) $(RANDOM_MODS) $(TLS_MODS) $(NET_MODS) $(TIMER_MODS) $(COV_MODS)

test-basic: $(TEST_DEPS)
	@bash test/run_basic_tests.sh

# GC stress runs both VMs: TA_GC_STRESS is a ta.h heap knob, and tavm uses
# that same heap/GC (issue #249). Measured on the backend pass: 18/18 pass, slowest
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

# kernel 自足纪律门（kl-kernel 步 1）：lib/*.ta + lib/bootstrap/*.ta 逐单元
# 冷 sig-cache 独立 build + 直 declaration 扫描。放 test 链（backend-gate 是
# Linux-only 的重语义门；这是结构门，双 OS 的 test job 都该看守）。
test-unit-gate: $(TEST_DEPS)
	@sh test/run_module_gate.sh

# 步 2 逐模块 .kl + 链接门：链接 == 整 .kl 逐字节 + --from-kl 闭环。
# 同样放 test 链（结构/接缝门，双 OS 都该看守；backend-gate 留给重语义门）。
test-kl-link: $(TEST_DEPS)
	@sh test/run_kl_link_gate.sh

# 步 3 .kl 语义正例：--from-kl 编译 -> 运行 -> cmp（test/kl/*.kl）。
test-kl-sem: $(TEST_DEPS)
	@sh test/run_kl_sem_tests.sh


test-1m-actor: $(TEST_DEPS)
	@bash test/run_1m_actor.sh

test-gc-long: $(TEST_DEPS)
	@bash test/run_gc_long_tests.sh

# clang-format 版本护栏的正/负例（issue #263）：PATH shim 模拟错版本/对版本，
# 断言 guard 在任何格式化动作之前拦截错版本且不碰工作区。依赖 TEST_DEPS：
# 嵌套 make fmt 里的 ./tinyactor fmt 需要 tavm + driver.tabc（7b-3 fmt 再宿主）
# 与运行期 .so；TEST_DEPS 已全部包含。
test-fmt-guard: $(TEST_DEPS)
	@bash test/run_fmt_guard_tests.sh






test: test-basic test-gc test-actor test-module test-compiler test-example test-cli test-unit-gate test-kl-link test-kl-sem test-fmt-guard

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
#
# Re-cal 2026-10-17 against the post-sink universe (str + file + os + net +
# timer sunk into non-instrumented lib/*.c, so tavm's own 11 files are the
# whole universe — see the COV block above). Both platforms were measured
# with the same target on the same commit:
#
#   macOS  (Apple clang, local)  84.18%  3114/3699
#   Linux  (ubuntu runner, CI)   84.40%  3137/3717
#
# Issue #250's ~2pp platform spread (macOS 77.79% vs Linux 79.83%) is gone,
# and its cause is now identified: the platform-conditional blocks of the
# then-instrumented src/net.c / tls.c / file.c / os.c — exactly the files the
# sinks moved out of the universe. What remains is 0.22pp at most, with two
# sources, both structural rather than a measurement bug:
#   * Linux LF is 18 lines larger — #ifdef-only lines in api/main/prof/
#     scheduler/vm, a few of which are covered.
#   * ta_inline.h: same 396 lines, Linux misses 7 fewer. The likely cause is
#     the clang version's inlining/line mapping of the static inline helpers:
#     the skip list is NOT a factor (SKIP_LIST in test/lib.sh is
#     unconditional, so both platforms skip the same 4 tests). Either way it
#     is measurement-side, not a difference in the code under test.
# So one floor fits both platforms; 81 = the WORSE reading (macOS, verified
# reproducible: two independent local runs both read 3114/3699) minus 3.2pp,
# i.e. ~118 lines of slack — ample for the ≤0.22pp platform spread and for
# run-to-run wobble (CI line counts have read 3134 and 3137 of 3717).
#
# History: 78 was the post-T4.5 floor (21-file / 5762-line universe, first
# readings Linux 77.94% / macOS 78.20%, later macOS 81.07%). The str sink
# (~655 well-covered lines) dropped the aggregate below it, so 75 was set
# 2026-10-12 as a temporary floor to unblock that change. The subsequent
# file/os/net/timer sinks raised the ratio instead (they removed
# platform-heavy lines), which is why this re-cal lands ABOVE 78.
#
# Ratchet policy: raise over time as tests improve; the plan is 85+ (1pp ≈ 37
# lines). Next step: 82-83 once the worse platform reads >= 85%. Bump the
# committed default; to preview a future threshold locally:
#   make coverage COV_MIN=85
#
# The floor is a single number on purpose: local macOS and CI run the same
# `make coverage` target and read within 0.15pp of each other, so "local
# FAILs, CI PASSes" (issue #250) can only mean a genuinely uncovered change.
COV_MIN      ?= 81
# %p keeps one .profraw per VM process. COV=1 builds exactly one
# instrumented binary — tavm (via `all`); the lib/*.c modules are compiled
# without instrumentation (see the COV block above), so tavm is the whole
# coverage universe.
COV_RUN_ENV := LLVM_PROFILE_FILE="$(CURDIR)/coverage/profraw/tavm-%p.profraw"

# C implementation coverage via LLVM instrumentation.
#
# test-bootstrap 也在列（C2 起）：编译半程的**重活**（driver 全闭包 + 292 KB
# .tabc + 420 KB .kl 文本）只在 G-B/G-D 里跑，而这类长跑把 tavm 自身
# 分配器/GC/解释器的冷路径也点亮了——覆盖率能看见它；`.kl` 路（--emit-kl /
# --from-kl）是编译路径上的真实执行面，不放进覆盖率跑就等于新加的门在覆盖率
# 宇宙里没跑过（lib/*.c 不插桩，COV=1 的宇宙只有 tavm 本身）。
test-cov:
	$(MAKE) clean
	$(COV_RUN_ENV) $(MAKE) COV=1 test test-bootstrap

# coverage: run the suite under COV=1, then gate on line coverage.
#
# Exactly one binary carries instrumentation (tavm — see COV_RUN_ENV
# above), so the lcov export/report is that binary alone: every instrumented
# file it maps (src/*.c, ta.h/ta_inline.h, src/main.c) lands in
# $(COV_LCOV) exactly once and the LF/LH sum the gate reads below covers the
# whole instrumented universe. The ignore regex drops build products
# (obj_/).
coverage: test-cov
	@command -v llvm-profdata >/dev/null 2>&1 || { echo "llvm-profdata not found (install Homebrew LLVM; it also provides the clang used for the COV build)" >&2; exit 1; }
	@command -v $(COV_TOOL) >/dev/null 2>&1 || { echo "$(COV_TOOL) not found in PATH" >&2; exit 1; }
	llvm-profdata merge -sparse coverage/profraw/*.profraw -o $(COV_PROFDATA)
	$(COV_TOOL) export tavm -instr-profile=$(COV_PROFDATA) -format=lcov \
		-ignore-filename-regex='(^|/)obj_/' > $(COV_LCOV)
	$(COV_TOOL) report tavm -instr-profile=$(COV_PROFDATA) \
		-ignore-filename-regex='(^|/)obj_/'
	@line_pct=$$(awk -F: '/^LH:/{lh+=$$2} /^LF:/{lf+=$$2} END { if (lf > 0) printf "%.2f", lh * 100 / lf; else print "0" }' $(COV_LCOV)); \
	gate_fail=$$(awk -v p="$$line_pct" -v min="$(COV_MIN)" 'BEGIN { print (p + 0 < min) ? 1 : 0 }'); \
	echo "LINE COVERAGE: $$line_pct% (gate: >= $(COV_MIN)%)"; \
	if [ "$$gate_fail" = "1" ]; then \
		echo "COVERAGE GATE FAILED: $$line_pct% < $(COV_MIN)% — add tests before merging" >&2; \
		exit 1; \
	fi; \
	echo "COVERAGE GATE PASSED"
		@echo "COVERAGE OK: report above; lcov written to $(COV_LCOV)"

# ============================================================
# TA program coverage (`make coverage-ta`)
#
# `--cov` 是编译期插桩（lib/bootstrap/covinst.ta）：函数入口 + 每条语句 /
# match-arm 各拿一个计数器，id -> file:line 的侧表写进 <out>.covmap；
# 运行时 C 侧 sink（lib/cov.c）记账，进程退出时落 dump（TA_COV_DUMP_DIR）。
# 报告 = covmap × dump 的连接（tools/coverage_ta.py），HTML 视图
# tools/coverage_html.ta。
#
# 一次执行两分数据，判据与数字永不漂移（同 `make coverage` 的约定）：
#   * compiler 组：插桩版 driver 自己跑整套测试的计数 —— TA_DRIVER_BC 指到
#     插桩镜像，每个编译进程退出时 dump 到 $run_dir/programs/compiler-dumps；
#   * program 组：每个测试程序各自的插桩镜像 —— TA_COV_MAP_DIR 打开收集，
#     tinyactor 按镜像分目录收 map/dump（coverage id 是镜像局部的）。
# 两组按源码位置合并（tools/merge_coverage_ta.py）后再出报告。
#
# 硬门：函数入口覆盖率（报告第一行）>= COV_TA_MIN%。棘轮策略：随测试补强
# 往上抬。本地预演更高的阈值：make coverage-ta COV_TA_MIN=90
#
# HTML 视图的源码清单（sources.txt）只收「本检出读过/该读的手写 .ta」：
#   * 本检出的绝对路径 —— 与 covmap 里记录的路径同形，测过的文件才能对上源码；
#   * 排除兄弟检出（.worktrees/，同一份仓库的其它 checkout）；
#   * 排除生成物（build/）与 kernfuzz 冻结语料（test/kernfuzz-frozen/，
#     tools/kernfuzz 生成的负例数据，不是给人读的源码）—— 它们占全仓 .ta 的
#     九成文件量，收进来只会让 HTML 慢一个数量级。
# 判据只吃 covmap（测到的位置），清单仅影响 HTML 呈现。
# ============================================================
COV_TA_MIN   ?= 85
.PHONY: coverage-ta
coverage-ta: $(TEST_DEPS)
	@set -e; \
	mkdir -p coverage/ta; \
	run_dir=$$(mktemp -d coverage/ta/run.XXXXXX); \
	mkdir -p "$$run_dir/tmp" "$$run_dir/programs"; \
	find "$(CURDIR)" -type f -name '*.ta' \
		-not -path '*/.git/*' -not -path '*/coverage/*' \
		-not -path '*/.worktrees/*' -not -path '*/build/*' \
		-not -path '*/kernfuzz-frozen/*' | sort > "$$run_dir/sources.txt"; \
	cp "$$run_dir/sources.txt" coverage/ta/coverage-ta-sources.txt; \
	./tinyactor build --cov lib/bootstrap/driver.ta "$(CURDIR)/$$run_dir/bootstrap.tabc"; \
	TA_DRIVER_BC="$(CURDIR)/$$run_dir/bootstrap.tabc" TA_COV_MAP_DIR="$(CURDIR)/$$run_dir/programs" TMPDIR="$(CURDIR)/$$run_dir/tmp" timeout 900 $(MAKE) test; \
	python3 test/test_coverage_ta.py; \
	python3 tools/merge_coverage_ta.py "$(CURDIR)/$$run_dir/bootstrap.tabc.covmap" "$(CURDIR)/$$run_dir/programs/compiler-dumps" "$(CURDIR)/$$run_dir/programs" "$(CURDIR)/$$run_dir/merged.covmap" "$(CURDIR)/$$run_dir/merged-dumps"; \
	python3 tools/coverage_ta.py "$(CURDIR)/$$run_dir/merged.covmap" "$(CURDIR)/$$run_dir/merged-dumps" "$(CURDIR)/$$run_dir/report.txt"; \
	cp "$(CURDIR)/$$run_dir/report.txt" coverage/ta/report.txt; \
	TA_ACTOR_HEAP=268435456 ./tinyactor run tools/coverage_html.ta "$(CURDIR)/$$run_dir/merged.covmap" "$(CURDIR)/$$run_dir/merged-dumps" coverage/ta/coverage.html coverage/ta/misses.txt "$(CURDIR)/$$run_dir/sources.txt"; \
	cat "$(CURDIR)/$$run_dir/report.txt"; \
	echo "html report: coverage/ta/coverage.html (open in a browser)"; \
	echo "miss list:    coverage/ta/misses.txt (grep-able fn/line misses)"; \
	awk -v min="$(COV_TA_MIN)" 'NR == 1 { split($$4, coverage, "/"); pct = coverage[1] / coverage[2] * 100; printf "TA COVERAGE GATE: %.2f%% (minimum: %d%%)\n", pct, min; if (pct < min) exit 1 } END { if (NR == 0) exit 1 }' "$(CURDIR)/$$run_dir/report.txt"

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
# The build line must include tavm: default VM is the backend chain since #246, clean
# wipes the binary, and without it every runner fails with "tavm not found".
# The backend runtime half then runs under ASAN/TSAN too (run_tavm spawns tavm).
#
# driver prereq（同 TEST_DEPS 的 file 目标）：fresh checkout 下 .tabc 缺失，
# run_tavm 懒重建会当场重建种子，CI 2 核 >180s 必被
# per-test 超时杀 → driver 永远建不出 → 每个测试重复冷重建直至 45min job 上限
# （coverage-c 同款死亡螺旋，2026-10-08 sanitizer job 首跑实测）。prereq 在
# recipe 的 clean 之前执行，clean 不删 driver 产物，plain 工具链建的 .tabc
# 与 sanitizer 无关（产物字节码相同；编译/运行半程都跑 tavm——ASAN=1 下
# tavm 即 asan 构建，sanitizer 覆盖比 7b-2 前更完整）。
# tavm 是 _asan/_tsan 标签构建（TA_MOD_TAG → 加载 lib/<mod>_asan.dylib 等），
# 但 SAN 只在下面的递归 make 里生效——若不在这里显式补 MOD_SUFFIX，export 的
# 就是空串，run_crash_tests.sh 会把 stub 模块命名为无标签的 lib/nodyslib.dylib，
# 与加载器查找的 lib/nodyslib_asan.dylib 错位（dylib-missing 误报）。
test-asan: export MOD_SUFFIX := _asan
test-asan: lib/bootstrap.tabc
	$(MAKE) clean
	$(MAKE) ASAN=1 all
	bash test/run_basic_tests.sh
	bash test/run_gc_tests.sh
	bash test/run_actor_tests.sh
	bash test/run_module_tests.sh
	bash test/run_compiler_tests.sh
	bash test/run_bootstrap_tests.sh
	bash test/run_example_tests.sh

test-tsan: export MOD_SUFFIX := _tsan
test-tsan: lib/bootstrap.tabc
	$(MAKE) clean
	$(MAKE) TSAN=1 all
	bash test/run_basic_tests.sh
	bash test/run_gc_tests.sh
	bash test/run_actor_tests.sh
		# json-mpc takes 169s under TSAN (vs 20s plain); 60s budget made it the
	# only flake in the suite — the widening now lives in
	# run_module_tests.sh (any instrumented build, MOD_SUFFIX non-empty).
	bash test/run_module_tests.sh
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

# tavm_asan: the recursive ASAN=1 invocation (obj_asan/ is separate from the
# plain obj dir, so the two builds coexist).  Built UNCONDITIONALLY before
# every kernfuzz run — make's dependency scan is the staleness gate, so a VM
# source change rebuilds it and an up-to-date binary costs a no-op scan.
# (The old 'only when missing' policy let a stale ASan VM run new-format
# bytecode: nightly 2026-09-29 produced 1082 phantom findings before the
# silent exit-0 / garbage-message mismatch was traced to the binary lag.)
# It is also the morph tavm arm's ASan base (tavm_arm.py), same staleness
# rationale, same separate-output-name rule.
# The driver file target is the compile half's staleness gate.
# lib/sexp_asan.* joins the line because the driver dlopens the sexp
# module at startup — missing → the first sexp.* call fails loudly
# ("cannot load C module 'sexp' from lib/sexp_<tag>.dylib", see TEST_DEPS'
# note and main.c find_cfunc_autoload); before that degradation it silently miscompiled findings.
kernfuzz-fast: tinyactor tavm lib/bootstrap.tabc
	@$(MAKE) --no-print-directory ASAN=1 tavm_asan lib/sexp_asan.$(HTTP_EXT) || exit 1;
		KERNFUZZ_PROGRESS=1 KERNFUZZ_FAST_SCALE=$${KERNFUZZ_FAST_SCALE:-0.4} python3 -u tools/kernfuzz/fast.py

# Regenerate the frozen tc-negative snapshot from the fixed seed list
# (commit the result; fast ring only replays it).
kernfuzz-freeze-tc: tinyactor tavm lib/bootstrap.tabc
	@$(MAKE) --no-print-directory ASAN=1 tavm_asan lib/sexp_asan.$(HTTP_EXT) || exit 1;
	python3 tools/kernfuzz/fast.py freeze-tc

# Verify regenerated frozen AST snapshots match the committed corpus.
# 断言用 `git status --porcelain -- test/kernfuzz-frozen/` 非空即红：参照系是
# HEAD（已提交状态），因此改过的快照、删掉的快照、以及 `git diff` 看不见的
# 「新语料漏入库的快照」全都算。新语料冻结流程：跑本目标 → git add
# test/kernfuzz-frozen/ → commit，之后复跑本目标才会绿。
# $(DRIVER_DEPS)：gate = `tinyactor run` ×83（编译半程 dlopen lib/sexp），
# fresh clone 直跑本目标必须先建出运行期模块——#275 把 $(TARGET) 换成
# tavm 时丢了这条传递依赖，coverage-c 83 语料全撞 arena abort。
kernfuzz-snapshot-check: tinyactor $(DRIVER_DEPS)
	@guile tools/kernfuzz/snapshot.scm || exit 1
	@if [ -n "$$(git status --porcelain -- test/kernfuzz-frozen/)" ]; then \
		echo "冻结快照与已提交状态（HEAD）不一致 —— 重生成后 git add + commit test/kernfuzz-frozen/：" >&2; \
		git status --porcelain -- test/kernfuzz-frozen/ >&2; \
		exit 1; \
	fi

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
#        The run pins NWORKERS to the host core count (mt-revival.md §6 D5):
#        with the default single worker the pool code never runs, so this
#        ring would only ever see main × io_poller × timer races.
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

kernfuzz-nightly: tinyactor tavm lib/bootstrap.tabc
	@$(MAKE) --no-print-directory ASAN=1 tavm_asan lib/sexp_asan.$(HTTP_EXT) || exit 1;
	KERNFUZZ_NIGHTLY_SCALE=$${KERNFUZZ_NIGHTLY_SCALE:-1.0} python3 tools/kernfuzz/nightly.py

# Bootstrap（7c-1 换锚）：旧链「tavm + bootstrap.tabc 重编 build.ta」随
# codegen/.tabc 世界删除；新自举锚 = 入库的 bootstrap.tabc 种子。
# 本目标 = 用现有种子自重建 driver 全闭包并原地写回：改过 lib/bootstrap/*.ta
# 或 test/backend/*.ta 源码后跑它，产出新种子提交入库；固定点 gate 由
# `make test-bootstrap` 把重建产物与入库种子逐字节比对（连跑两遍产物
# 一致 = fixed point，AGENTS 约定的语义原样保留）。
# 同一趟里也写 .kl 快照（lib/bootstrap/driver.kl，本地产物不入 git）：它与种子
# **同一次重建**产出，所以共用这一条 recipe，且两者都在编译成功后才
# `mv` 落位（失败留下的是旧的一对，不会出现「新文本 + 旧种子」的错位）。
# 两个产物各有自己的规则 ⇒ `-j` 下这条 recipe 可能被并行调起两次，所以临时
# 路径带 PID 后缀（共用固定名会互相覆写）；内容确定，谁后 `mv` 都一样。
#
# driver 镜像宿主在安装树里跑（link 期 dlopen lib/ 相对 CWD，同 CLI 的
# build_tavm 约定；参数已是绝对路径，换 CWD 无副作用）；env -u TA_PROJECT_ROOT
# 与 tinyactor 的自重建（ensure_tavm_driver）同一约定：编 driver 自己是
# **安装树内**的构建，用户 shell 里导出的 TA_PROJECT_ROOT 不得把它自己的
# bootstrap/* 指到别的项目去；模块解析回落到 cwd 相对的 vendor/ = 本仓库
# （docs/modules.md 的 vendor 根表）。
#
# A3 pipeline-safety: a pipe (`make bootstrap 2>&1 | tail -1`) reports the
# LAST command's status (tail → 0), masking a real failure. The recipe never
# trusts a pipeline exit code; it verifies the real compile result internally
# (artifact non-empty before mv) and prints an unambiguous verdict as its
# LAST line, so a piped `tail -1` still shows the truth. Callers that need a
# guaranteed-correct status must use `set -o pipefail` (GitHub Actions does
# by default) or PIPESTATUS.
bootstrap: $(DRIVER_DEPS)
		@bc=lib/bootstrap.tabc; \
	kl=lib/bootstrap/driver.kl; \
	tmpbc=lib/.bootstrap.rebuild.$$$$; \
	tmpkl=lib/.bootstrap.rebuild.kl.$$$$; \
	test -s $$bc || { echo "BOOTSTRAP FAILED: seed $$bc missing (入库产物) — git checkout -- $$bc" >&2; exit 1; }; \
	rm -f $$tmpbc $$tmpkl; \
	env -u TA_PROJECT_ROOT ./tavm -q $$bc --emit-kl $$tmpkl lib/bootstrap/driver.ta $$tmpbc "" || { rm -f $$tmpbc $$tmpkl; echo "BOOTSTRAP FAILED: self-rebuild errored" >&2; exit 1; }; \
	test -s $$tmpbc || { rm -f $$tmpkl; echo "BOOTSTRAP FAILED: self-rebuild produced no artifact" >&2; exit 1; }; \
	test -s $$tmpkl || { rm -f $$tmpbc; echo "BOOTSTRAP FAILED: self-rebuild wrote no .kl snapshot" >&2; exit 1; }; \
	mv -f $$tmpbc $$bc; \
	mv -f $$tmpkl $$kl; \
	echo "BOOTSTRAP OK: wrote $$bc + $$kl"

# ============================================================
# Formatting targets
#   make fmt       — format all C/C++ (clang-format) and lib/*.ta (tinyactor)
#   make fmt-check — verify both are properly formatted (exit 1 if not)
#
# 版本护栏（issue #263）：CI 的 fmt-check 只在 macOS job 跑，钉 brew
# llvm@18（18.1.8）。PATH 里另一个 major 版本跑一次 make fmt 就会把
# vm.c 宏续行 \ 的列对齐重写成纯空白 churn。因此 fmt/fmt-check
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

fmt: fmt-version-check tinyactor tavm lib/bootstrap.tabc
	@find . -path "./.tinyactor-build*" -prune -o -type f \( -name "*.c" -o -name "*.h" -o -name "*.cpp" -o -name "*.hpp" \) \
		-not -path "./.git/*" -not -path "./.vscode/*" \
		-exec clang-format -i {} \;
	@for f in lib/*.ta lib/bootstrap/*.ta; do ./tinyactor fmt "$$f"; done
	@echo "C/C++ and lib/*.ta formatted"

fmt-check: fmt-version-check tinyactor tavm lib/bootstrap.tabc
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
