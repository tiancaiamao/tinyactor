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

.PHONY: all clean test test-asan test-tsan bootstrap bootstrap-selfhost

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(RDYNAMIC) -o $@ $(OBJ) -lpthread $(LDLIBS)

$(OBJ_DIR)/%.o: src/%.c ta.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR):
	mkdir -p $@

# Shared library for dynamic C module loading (optional, example: http.so)
lib/%.so: lib/%.c ta.h
	$(CC) $(CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< -lpthread $(LDLIBS)

lib/%.dylib: lib/%.c ta.h
	$(CC) $(CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< -lpthread $(LDLIBS)

clean:
	rm -rf $(OBJ) tavm tavm_asan tavm_tsan obj_asan obj_tsan

# Test suite uses the tinyactor shell script + tavm binary
test: $(TARGET) tinyactor
	@echo "Running all tests..."
	@bash test/run_all_tests.sh

test-asan:
	$(MAKE) clean
	$(MAKE) ASAN=1 test

test-tsan:
	$(MAKE) clean
	$(MAKE) TSAN=1 test

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