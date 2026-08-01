CC      = cc
UNAME_S := $(shell uname -s)

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
  override SAN_CFLAGS  := -fsanitize=address -fno-omit-frame-pointer -O1 -g
  override SAN_LDFLAGS := -fsanitize=address
else ifdef TSAN
  override SAN := tsan
  override SAN_CFLAGS  := -fsanitize=thread -fno-omit-frame-pointer -O1 -g
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

# Shared library for dynamic C module loading (optional, example: http.so)
# macOS uses .dylib, Linux uses .so
ifeq ($(UNAME_S),Darwin)
HTTP_LIB = lib/http.dylib
else
HTTP_LIB = lib/http.so
endif
lib/%.so: lib/%.c ta.h
	$(CC) $(CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< -lpthread $(LDLIBS)

lib/%.dylib: lib/%.c ta.h
	$(CC) $(CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< -lpthread $(LDLIBS)

clean:
	rm -rf $(OBJ) tavm tavm_asan tavm_tsan obj_asan obj_tsan

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

test-basic: $(TARGET) tinyactor
	@bash test/run_basic_tests.sh

test-gc: $(TARGET) tinyactor
	@bash test/run_gc_tests.sh

test-actor: $(TARGET) tinyactor
	@bash test/run_actor_tests.sh

test-module: $(TARGET) tinyactor
	@bash test/run_module_tests.sh

test-compiler: $(TARGET) tinyactor
	@bash test/run_compiler_tests.sh

test-bootstrap: $(TARGET) tinyactor
	@bash test/run_bootstrap_tests.sh

test-example: $(TARGET) tinyactor
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
bootstrap: tavm tinyactor
	./tinyactor build lib/driver.ta lib/bootstrap.tabc
	@echo "wrote lib/bootstrap.tabc"

# Self-hosting: use TA compiler to emit bootstrap_selfhost.tabc,
# then verify it matches bootstrap.tabc (fixed point).
bootstrap-selfhost: bootstrap
	./tinyactor build lib/driver.ta lib/bootstrap_selfhost.tabc
	@echo "wrote lib/bootstrap_selfhost.tabc"
	@cmp lib/bootstrap.tabc lib/bootstrap_selfhost.tabc && echo "FIXED POINT VERIFIED" || echo "WARNING: fixed point mismatch!"

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