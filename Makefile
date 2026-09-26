CC ?= cc
UNAME_S := $(shell uname -s)

# ============================================================
# Shared library for dynamic C module loading
# macOS uses .dylib, Linux uses .so
#
# Each module is built per sanitizer configuration
# (lib/demo.dylib / lib/demo_asan.dylib / lib/demo_tsan.dylib, ...), so a
# sanitizer build never overwrites — or leaves behind — the plain module
# that the regular tavm dlopens at startup. (A TSAN-instrumented
# lib/demo.dylib used to abort the plain tavm with "Interceptors are not
# working", failing every test.)
# ============================================================
ifeq ($(UNAME_S),Darwin)
HTTP_EXT = dylib
else
HTTP_EXT = so
endif

# ============================================================
# Sanitizer support
#   ASAN=1 make tavm     → build with AddressSanitizer
#   TSAN=1 make tavm     → build with ThreadSanitizer
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
# tavm_cov that loads its own lib/demo_cov module and never touches the
# plain tavm.
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
  TARGET   := tavm_cov
  OBJ_DIR  := obj_cov
  CFLAGS    = -Wall -Wextra -std=c99 -I. $(COV_CFLAGS)
  LDLIBS    = $(COV_LDFLAGS)
  # C modules (lib/demo.c, lib/math.c, ...) are NOT instrumented under COV:
  # a dlopen'd library pulls in its own profile runtime and its counters
  # never flush into the main executable's merged profraw. They keep the
  # module tag (so tavm_cov loads the _cov variant) but plain flags.
  MOD_CFLAGS = -Wall -Wextra -std=c99 -O2 -I. -DTA_MOD_TAG=$(COV_TAG)
  MOD_LDLIBS =
else ifdef SAN
  TARGET   := tavm_$(SAN)
  OBJ_DIR  := obj_$(SAN)
  CFLAGS    = -Wall -Wextra -std=c99 -I. $(SAN_CFLAGS)
  LDLIBS    = $(SAN_LDFLAGS)
  MOD_CFLAGS = $(CFLAGS)
  MOD_LDLIBS = $(LDLIBS)
else
  TARGET   := tavm
  OBJ_DIR  := src
  CFLAGS    = -Wall -Wextra -std=c99 -O2 -I.
  LDLIBS    =
  MOD_CFLAGS = $(CFLAGS)
  MOD_LDLIBS = $(LDLIBS)
endif

# Shared module output — one per build config (plain / _asan / _tsan / _cov),
# so a sanitizer/coverage build never overwrites the module the plain tavm loads.
DEMO_LIB := lib/demo$(COV_TAG:%=_%)$(SAN:%=_%).$(HTTP_EXT)
MATH_LIB := lib/math$(COV_TAG:%=_%)$(SAN:%=_%).$(HTTP_EXT)
TIME_LIB := lib/time$(COV_TAG:%=_%)$(SAN:%=_%).$(HTTP_EXT)
BUFFER_LIB := lib/buffer$(COV_TAG:%=_%)$(SAN:%=_%).$(HTTP_EXT)
PROCESS_LIB := lib/process$(COV_TAG:%=_%)$(SAN:%=_%).$(HTTP_EXT)

ifdef GC_DEBUG
  CFLAGS += -DGC_DEBUG=1
endif

# ============================================================
# OpenSSL for the static tls module (stdlib-port-plan Phase 6 #1).
#   macOS: Homebrew openssl@3 (keg-only, not on the default include/lib
#          search path) — fall back to pkg-config openssl when brew (or
#          the formula) is absent.
#   Linux: system libssl-dev via pkg-config, or plain -lssl -lcrypto.
# The module is compiled into tavm itself, so these flags apply to the
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

SRC     = src/val.c src/vm.c src/builtin.c src/scheduler.c src/timer.c src/gc.c src/api.c src/net.c src/tls.c src/file.c src/os.c src/buf.c src/str.c src/num.c src/encoding.c src/random.c src/prof.c src/tavm.c
OBJ     = $(SRC:src/%.c=$(OBJ_DIR)/%.o)

.PHONY: all clean test test-basic test-gc test-actor test-module test-compiler \
        test-bootstrap test-example test-cli test-gc-asan test-gc-tsan \
        test-asan test-tsan test-cov coverage \
        bootstrap benchmark benchmark-regression \
        benchmark-clean fmt kernfuzz-fast kernfuzz-freeze-tc \
        kernfuzz-nightly

# Default build ships the complete C-module set: a bare `make clean; make`
# must leave a runtime where scripts can actually call demo/math/time/
# buffer/process (the dylibs are lazy-loaded; a missing one makes the call
# silently push nil instead of erroring).
all: $(TARGET) $(DEMO_LIB) $(MATH_LIB) $(TIME_LIB) $(BUFFER_LIB) $(PROCESS_LIB)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(RDYNAMIC) -o $@ $(OBJ) -lpthread $(LDLIBS)

HDRS = ta.h ta_inline.h

$(OBJ_DIR)/%.o: src/%.c $(HDRS) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR):
	mkdir -p $@

# E3: demo module — minimal C module template (docs/c-module.md).
# One output per build config (plain / _asan / _tsan / _cov), so a sanitizer
# or coverage build never overwrites the module the plain tavm loads.
DEMO_MODS = lib/demo.$(HTTP_EXT) lib/demo_asan.$(HTTP_EXT) lib/demo_tsan.$(HTTP_EXT) lib/demo_cov.$(HTTP_EXT)
$(DEMO_MODS): lib/demo.c $(HDRS)
	$(CC) $(MOD_CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< $(MOD_LDLIBS)

# math / time modules (stdlib-port-plan Workstream A / C) — same lazy-dylib
# pattern as demo. -lm: libm symbols must resolve at dlopen(RTLD_NOW) even
# on Linux where the plain tavm link does not export them transitively.
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

clean:
	rm -rf $(OBJ) tavm tavm_asan tavm_tsan tavm_cov obj_asan obj_tsan obj_cov coverage lib/*.so lib/*.dylib

# ============================================================
# Benchmark targets
#   make benchmark           — run all benchmarks
#   make benchmark-regression — run benchmarks and check for regression
#   make benchmark-clean     — clean benchmark results
# ============================================================

benchmark: $(TARGET) tinyactor
	@bash benchmark/run_benchmarks.sh

benchmark-regression: $(TARGET) tinyactor
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
#   make check-opcodes  — opcode numbering mirrors (no compiler/VM involved)
# ============================================================

TEST_DEPS = $(TARGET) tinyactor $(DEMO_MODS) $(MATH_MODS) $(TIME_MODS) $(BUFFER_MODS) $(PROCESS_MODS)

test-basic: $(TEST_DEPS)
	@bash test/run_basic_tests.sh

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


# The opcode numbers are mirrored by hand in ta.h, lib/bootstrap/codegen.ta,
# src/vm.c (goto table + handler labels) and src/api.c (instr_len, keyed by row
# order). A miss is silent at compile time, so this static check runs before
# the categories: it needs no build, and it names every differing entry.
check-opcodes:
	@python3 test/check_opcode_mirrors.py

test: check-opcodes test-bootstrap test-basic test-gc test-actor test-module test-compiler test-example test-cli

# ============================================================
# Coverage targets
#   make coverage — one-stop: clean, COV=1 build, run `make test` in
#                   parallel under the instrumented tavm_cov, merge
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
# spawns its own tavm process; LLVM_PROFILE_FILE uses %p so each process
# writes a separate .profraw that llvm-profdata merges afterwards.
# ============================================================
COV_TOOL     ?= llvm-cov
COV_PROFDATA ?= coverage/coverage.profdata
COV_LCOV     ?= coverage/coverage.lcov
# Hard CI gate: line coverage (LF/LH in the .lcov) must be >= COV_MIN%.
# Ratchet policy: raise this over time as tests improve — the plan is 85+.
# Bump the committed default; to preview a future threshold locally:
#   make coverage COV_MIN=85
COV_MIN      ?= 75
# %p keeps one .profraw per tavm process; TAVM= points both the test
# harness (test/lib.sh) and the tinyactor wrapper at the instrumented
# binary. COV=1 flips TEST_DEPS' $(TARGET) to tavm_cov automatically.
COV_RUN_ENV := LLVM_PROFILE_FILE="$(CURDIR)/coverage/profraw/tavm-%p.profraw" TAVM="$(CURDIR)/tavm_cov"

test-cov:
	$(MAKE) clean
	$(COV_RUN_ENV) $(MAKE) COV=1 test

coverage: test-cov
	@command -v llvm-profdata >/dev/null 2>&1 || { echo "llvm-profdata not found (install Homebrew LLVM; it also provides the clang used for the COV build)" >&2; exit 1; }
	@command -v $(COV_TOOL) >/dev/null 2>&1 || { echo "$(COV_TOOL) not found in PATH" >&2; exit 1; }
	llvm-profdata merge -sparse coverage/profraw/*.profraw -o $(COV_PROFDATA)
	$(COV_TOOL) export tavm_cov -instr-profile=$(COV_PROFDATA) -format=lcov \
		-ignore-filename-regex='(^|/)obj_/' > $(COV_LCOV)
	$(COV_TOOL) report tavm_cov -instr-profile=$(COV_PROFDATA) \
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

# Sanitizer targets — only for GC tests
test-gc-asan:
	$(MAKE) clean
	$(MAKE) ASAN=1
	TAVM=./tavm_asan bash test/run_gc_tests.sh

test-gc-tsan:
	$(MAKE) clean
	$(MAKE) TSAN=1
	TAVM=./tavm_tsan bash test/run_gc_tests.sh

# Legacy full-suite sanitizer targets (run everything under sanitizer)
test-asan:
	$(MAKE) clean
	$(MAKE) ASAN=1
	TAVM=./tavm_asan bash test/run_basic_tests.sh
	TAVM=./tavm_asan bash test/run_gc_tests.sh
	TAVM=./tavm_asan bash test/run_actor_tests.sh
	TAVM=./tavm_asan bash test/run_module_tests.sh
	TAVM=./tavm_asan bash test/run_compiler_tests.sh
	TAVM=./tavm_asan bash test/run_bootstrap_tests.sh
	TAVM=./tavm_asan bash test/run_example_tests.sh

test-tsan:
	$(MAKE) clean
	$(MAKE) TSAN=1
	TAVM=./tavm_tsan bash test/run_basic_tests.sh
	TAVM=./tavm_tsan bash test/run_gc_tests.sh
	TAVM=./tavm_tsan bash test/run_actor_tests.sh
	TAVM=./tavm_tsan bash test/run_module_tests.sh
	TAVM=./tavm_tsan bash test/run_compiler_tests.sh
	TAVM=./tavm_tsan bash test/run_bootstrap_tests.sh
	TAVM=./tavm_tsan bash test/run_example_tests.sh

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

# tavm_asan: built once via the recursive ASAN=1 invocation (obj_asan/ is
# separate from the plain obj dir, so the two builds coexist).  Only built
# when missing — refresh manually with 'ASAN=1 make tavm' after VM changes.
kernfuzz-fast: $(TARGET) tinyactor
	@if [ ! -x ./tavm_asan ]; then \
		echo "== kernfuzz-fast: tavm_asan missing, building ASan base (one-time)"; \
		$(MAKE) --no-print-directory ASAN=1 tavm || exit 1; \
	fi
	KERNFUZZ_FAST_SCALE=$${KERNFUZZ_FAST_SCALE:-0.4} python3 tools/kernfuzz/fast.py

# Regenerate the frozen tc-negative snapshot from the fixed seed list
# (commit the result; fast ring only replays it).
kernfuzz-freeze-tc: $(TARGET) tinyactor
	@if [ ! -x ./tavm_asan ]; then \
		$(MAKE) --no-print-directory ASAN=1 tavm || exit 1; \
	fi
	python3 tools/kernfuzz/fast.py freeze-tc

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

kernfuzz-nightly: $(TARGET) tinyactor
	@if [ ! -x ./tavm_asan ]; then \
		echo "== kernfuzz-nightly: tavm_asan missing, building ASan base (one-time)"; \
		$(MAKE) --no-print-directory ASAN=1 tavm_asan || exit 1; \
	fi
			KERNFUZZ_NIGHTLY_SCALE=$${KERNFUZZ_NIGHTLY_SCALE:-1.0} python3 tools/kernfuzz/nightly.py

# Bootstrap: compile driver.ta into bootstrap.tabc using the existing
# bootstrap.tabc (committed in git). Requires tavm and tinyactor.
#
# The TA compiler sources are declared as prerequisites so `make bootstrap`
# detects a stale bootstrap.tabc (source newer than artifact) and rebuilds —
# a silent stale artifact previously masked compile errors.
#
# A3 pipeline-safety: a pipe (`make bootstrap 2>&1 | tail -1`) reports the
# LAST command's status (tail → 0), masking a real failure. The recipe never
# trusts a pipeline exit code; it verifies the real compile result internally
# (artifact non-empty before mv) and prints an unambiguous verdict as its
# LAST line, so a piped `tail -1` still shows the truth. Callers that need a
# guaranteed-correct status must use `set -o pipefail` (GitHub Actions does
# by default) or PIPESTATUS.
TA_COMPILER_SRCS = lib/bootstrap/driver.ta lib/bootstrap/tokenizer.ta lib/bootstrap/parser.ta lib/bootstrap/codegen.ta lib/bootstrap/typecheck.ta lib/bootstrap/fmt.ta lib/bootstrap/modsig.ta

bootstrap: tavm tinyactor $(TA_COMPILER_SRCS)
	rm -f lib/bootstrap.tabc.tmp
	./tinyactor build lib/bootstrap/driver.ta lib/bootstrap.tabc.tmp
	@test -s lib/bootstrap.tabc.tmp || { echo "BOOTSTRAP FAILED: tinyactor build produced no artifact" >&2; exit 1; }
	@mv lib/bootstrap.tabc.tmp lib/bootstrap.tabc
	@echo "BOOTSTRAP OK: wrote lib/bootstrap.tabc"

# ============================================================
# Formatting targets
#   make fmt       — format all C/C++ (clang-format) and lib/*.ta (tinyactor)
#   make fmt-check — verify both are properly formatted (exit 1 if not)
# ============================================================
fmt: tinyactor lib/bootstrap.tabc
	@find . -type f \( -name "*.c" -o -name "*.h" -o -name "*.cpp" -o -name "*.hpp" \) \
		-not -path "./.git/*" -not -path "./.vscode/*" \
		-exec clang-format -i {} \;
	@for f in lib/*.ta lib/bootstrap/*.ta; do ./tinyactor fmt "$$f"; done
	@echo "C/C++ and lib/*.ta formatted"

fmt-check: tinyactor lib/bootstrap.tabc
	@echo "Checking code formatting..."
	@which clang-format > /dev/null || (echo "clang-format is not installed" && exit 1)
	@out="$$(find . -type f \( -name "*.c" -o -name "*.h" -o -name "*.cpp" -o -name "*.hpp" \) \
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
