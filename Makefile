CC      = cc
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

ifdef SAN
  TARGET   := tavm_$(SAN)
  OBJ_DIR  := obj_$(SAN)
  CFLAGS    = -Wall -Wextra -std=c99 -I. $(SAN_CFLAGS)
  LDLIBS    = $(SAN_LDFLAGS)
else
  TARGET   := tavm
  OBJ_DIR  := src
  CFLAGS    = -Wall -Wextra -std=c99 -O2 -I.
  LDLIBS    =
endif

# Shared module output — one per sanitizer config (plain / _asan / _tsan),
# so a sanitizer build never overwrites the module the plain tavm loads.
HTTP_LIB := lib/http$(SAN:%=_%).$(HTTP_EXT)

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

SRC     = src/val.c src/vm.c src/scheduler.c src/gc.c src/api.c src/net.c src/file.c src/buf.c src/str.c src/tavm.c
OBJ     = $(SRC:src/%.c=$(OBJ_DIR)/%.o)

.PHONY: all clean test test-basic test-gc test-gc-asan test-gc-tsan \
        test-actor test-module test-compiler test-bootstrap test-example \
        test-asan test-tsan bootstrap bootstrap-selfhost \
        benchmark benchmark-regression benchmark-clean \
        fmt

all: $(TARGET) $(HTTP_LIB)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(RDYNAMIC) -o $@ $(OBJ) -lpthread $(LDLIBS)

$(OBJ_DIR)/%.o: src/%.c ta.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR):
	mkdir -p $@

# Shared library for dynamic C module loading — one output per sanitizer
# config (plain / _asan / _tsan), all built from lib/http.c, so a
# sanitizer build never overwrites the module the plain tavm loads.
HTTP_MODS = lib/http.$(HTTP_EXT) lib/http_asan.$(HTTP_EXT) lib/http_tsan.$(HTTP_EXT)
$(HTTP_MODS): lib/http.c ta.h
	$(CC) $(CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< -lpthread $(LDLIBS)

clean:
	rm -rf $(OBJ) tavm tavm_asan tavm_tsan obj_asan obj_tsan lib/*.so lib/*.dylib

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

TEST_DEPS = $(TARGET) tinyactor $(HTTP_LIB)

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
# a silent stale artifact previously masked compile errors. Note the artifact
# mtime check is the gate; do NOT verify bootstrap success via `| tail`
# (pipes mask the exit code).
TA_COMPILER_SRCS = lib/driver.ta lib/tokenizer.ta lib/parser.ta lib/codegen.ta lib/typecheck.ta

bootstrap: tavm tinyactor $(TA_COMPILER_SRCS)
	./tinyactor build lib/driver.ta lib/bootstrap.tabc
	@echo "wrote lib/bootstrap.tabc"

# Self-hosting: use TA compiler to emit bootstrap_selfhost.tabc,
# then verify it matches bootstrap.tabc (fixed point).
# A mismatch is an ERROR (exit 1), not a warning: the fixed point is the
# project's core self-hosting guarantee and must gate CI.
bootstrap-selfhost: bootstrap
	./tinyactor build lib/driver.ta lib/bootstrap_selfhost.tabc
	@echo "wrote lib/bootstrap_selfhost.tabc"
	@cmp lib/bootstrap.tabc lib/bootstrap_selfhost.tabc && echo "FIXED POINT VERIFIED" || (echo "FIXED POINT MISMATCH!" && exit 1)

# ============================================================
# Formatting target
#   make fmt — format all C/C++ source files with clang-format
# ============================================================
fmt:
	@find . -type f \( -name "*.c" -o -name "*.h" -o -name "*.cpp" -o -name "*.hpp" \) \
		-not -path "./.git/*" -not -path "./.vscode/*" \
		-exec clang-format -i {} \;
	@echo "C/C++ code formatted with clang-format"

# ============================================================
# Format check target
#   make fmt-check — verify all files are properly formatted
# ============================================================
fmt-check:
	@echo "Checking code formatting..."
	@which clang-format > /dev/null || (echo "clang-format is not installed" && exit 1)
	@find . -type f \( -name "*.c" -o -name "*.h" -o -name "*.cpp" -o -name "*.hpp" \) \
		-not -path "./.git/*" -not -path "./.vscode/*" \
		-exec clang-format --dry-run --Werror {} \;
	@echo "All files are properly formatted."