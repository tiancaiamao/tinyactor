CC ?= cc
UNAME_S := $(shell uname -s)

# ============================================================
# Shared library for dynamic C module loading
# macOS uses .dylib, Linux uses .so
#
# The module is built per sanitizer configuration
# (lib/http.dylib / lib/http_asan.dylib / lib/http_tsan.dylib), so a
# sanitizer build never overwrites — or leaves behind — the plain module
# that the regular tavm dlopens at startup. (A TSAN-instrumented
# lib/http.dylib used to abort the plain tavm with "Interceptors are not
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
# tavm_cov that loads its own lib/http_cov module and never touches the
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
  # C modules (lib/http.c, lib/demo.c) are NOT instrumented under COV:
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
HTTP_LIB := lib/http$(COV_TAG:%=_%)$(SAN:%=_%).$(HTTP_EXT)

ifdef GC_DEBUG
  CFLAGS += -DGC_DEBUG=1
endif

# Linux needs -ldl for dlopen/dlsym; macOS has it in libSystem
ifneq ($(UNAME_S),Darwin)
LDLIBS += -ldl
endif
# Export symbols for dynamically loaded modules (needed on Linux for dlopen)
ifeq ($(UNAME_S),Linux)
RDYNAMIC = -Wl,--export-dynamic
else
# macOS: allow shared lib to have unresolved symbols resolved at load time
UNDEF_OK = -undefined dynamic_lookup
endif

SRC     = src/val.c src/vm.c src/scheduler.c src/gc.c src/api.c src/net.c src/file.c src/buf.c src/str.c src/prof.c src/tavm.c
OBJ     = $(SRC:src/%.c=$(OBJ_DIR)/%.o)

.PHONY: all clean test test-basic test-gc test-gc-asan test-gc-tsan \
        test-actor test-module test-compiler test-bootstrap test-example \
        test-asan test-tsan test-cov coverage \
        bootstrap bootstrap-selfhost \
        benchmark benchmark-regression benchmark-clean \
        fmt

all: $(TARGET) $(HTTP_LIB)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(RDYNAMIC) -o $@ $(OBJ) -lpthread $(LDLIBS)

$(OBJ_DIR)/%.o: src/%.c ta.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR):
	mkdir -p $@

# Shared library for dynamic C module loading — one output per build config
# (plain / _asan / _tsan / _cov), all built from lib/http.c, so a sanitizer
# or coverage build never overwrites the module the plain tavm loads.
HTTP_MODS = lib/http.$(HTTP_EXT) lib/http_asan.$(HTTP_EXT) lib/http_tsan.$(HTTP_EXT) lib/http_cov.$(HTTP_EXT)
$(HTTP_MODS): lib/http.c ta.h
	$(CC) $(MOD_CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< -lpthread $(MOD_LDLIBS)

# E3: demo module — minimal C module template (docs/c-module.md).
# One output per build config (plain / _asan / _tsan / _cov), same as http.
DEMO_MODS = lib/demo.$(HTTP_EXT) lib/demo_asan.$(HTTP_EXT) lib/demo_tsan.$(HTTP_EXT) lib/demo_cov.$(HTTP_EXT)
$(DEMO_MODS): lib/demo.c ta.h
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
# ============================================================

TEST_DEPS = $(TARGET) tinyactor $(HTTP_LIB) $(DEMO_MODS)

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


test: test-basic test-gc test-actor test-module test-compiler test-bootstrap test-example

# ============================================================
# Coverage targets
#   make coverage — instrumented (COV=1) build, run the full test suite,
#                   merge profiles and print a report + write .lcov
#   make test-cov — just the instrumented build + test run (no report)
#
# Requires llvm-profdata / llvm-cov (Homebrew LLVM) in PATH. Every test
# spawns its own tavm process; LLVM_PROFILE_FILE uses %p so each process
# writes a separate .profraw that llvm-profdata merges afterwards.
# ============================================================
COV_TOOL     ?= llvm-cov
COV_PROFDATA ?= coverage/coverage.profdata
COV_LCOV     ?= coverage/coverage.lcov
COV_RUNS     := test/run_basic_tests.sh test/run_gc_tests.sh test/run_actor_tests.sh \
                test/run_module_tests.sh test/run_compiler_tests.sh \
                test/run_bootstrap_tests.sh test/run_example_tests.sh

test-cov:
	$(MAKE) clean
	$(MAKE) COV=1 all $(DEMO_MODS)
	@for s in $(COV_RUNS); do \
		echo "=== coverage: $$s ==="; \
		LLVM_PROFILE_FILE="$(CURDIR)/coverage/profraw/tavm-%p.profraw" TAVM=./tavm_cov bash $$s || exit 1; \
	done

coverage: test-cov
	@command -v llvm-profdata >/dev/null 2>&1 || { echo "llvm-profdata not found (install Homebrew LLVM; it also provides the clang used for the COV build)" >&2; exit 1; }
	@command -v $(COV_TOOL) >/dev/null 2>&1 || { echo "$(COV_TOOL) not found in PATH" >&2; exit 1; }
	llvm-profdata merge -sparse coverage/profraw/*.profraw -o $(COV_PROFDATA)
	$(COV_TOOL) report tavm_cov -instr-profile=$(COV_PROFDATA) \
		-ignore-filename-regex='(^|/)obj_/'
	$(COV_TOOL) export tavm_cov -instr-profile=$(COV_PROFDATA) -format=lcov \
		-ignore-filename-regex='(^|/)obj_/' > $(COV_LCOV)
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
TA_COMPILER_SRCS = lib/driver.ta lib/tokenizer.ta lib/parser.ta lib/codegen.ta lib/typecheck.ta

bootstrap: tavm tinyactor $(TA_COMPILER_SRCS)
	rm -f lib/bootstrap.tabc.tmp
	./tinyactor build lib/driver.ta lib/bootstrap.tabc.tmp
	@test -s lib/bootstrap.tabc.tmp || { echo "BOOTSTRAP FAILED: tinyactor build produced no artifact" >&2; exit 1; }
	@mv lib/bootstrap.tabc.tmp lib/bootstrap.tabc
	@echo "BOOTSTRAP OK: wrote lib/bootstrap.tabc"

# Self-hosting: use TA compiler to emit bootstrap_selfhost.tabc,
# then verify it matches bootstrap.tabc (fixed point).
# A mismatch is an ERROR (exit 1), not a warning: the fixed point is the
# project's core self-hosting guarantee and must gate CI.
bootstrap-selfhost: bootstrap
	rm -f lib/bootstrap_selfhost.tabc
	./tinyactor build lib/driver.ta lib/bootstrap_selfhost.tabc
	@test -s lib/bootstrap_selfhost.tabc || { echo "SELFHOST FAILED: rebuild produced no artifact" >&2; exit 1; }
	@cmp lib/bootstrap.tabc lib/bootstrap_selfhost.tabc && echo "FIXED POINT VERIFIED" || { echo "FIXED POINT MISMATCH!" >&2; exit 1; }

# ============================================================
# Formatting targets
#   make fmt       — format all C/C++ (clang-format) and lib/*.ta (tinyactor)
#   make fmt-check — verify both are properly formatted (exit 1 if not)
# ============================================================
fmt: tinyactor lib/bootstrap.tabc
	@find . -type f \( -name "*.c" -o -name "*.h" -o -name "*.cpp" -o -name "*.hpp" \) \
		-not -path "./.git/*" -not -path "./.vscode/*" \
		-exec clang-format -i {} \;
	@for f in lib/*.ta; do ./tinyactor fmt "$$f"; done
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
	@for f in lib/*.ta; do \
		if ! ./tinyactor fmt --check "$$f" >/dev/null; then \
			echo "fmt-check FAILED: $$f (run 'make fmt')" >&2; \
			exit 1; \
		fi; \
	done
	@echo "All files are properly formatted."