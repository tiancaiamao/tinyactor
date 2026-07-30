CC      = cc
CFLAGS  = -Wall -Wextra -std=c99 -O2 -I.
LDLIBS  =
# Linux needs -ldl for dlopen/dlsym; macOS has it in libSystem
UNAME_S := $(shell uname -s)
ifneq ($(UNAME_S),Darwin)
LDLIBS += -ldl
endif
# Export symbols for dynamically loaded modules (default on macOS, explicit on Linux)
ifeq ($(UNAME_S),Linux)
RDYNAMIC = -Wl,--export-dynamic
else
# macOS: allow shared lib to have unresolved symbols resolved at load time
UNDEF_OK = -undefined dynamic_lookup
endif

SRC     = src/val.c src/vm.c src/scheduler.c src/gc.c src/api.c src/net.c src/file.c src/buf.c src/str.c src/tavm.c
OBJ     = $(SRC:.c=.o)

# Dynamically loaded C modules
DYNAMIC_MODS = lib/http.so

.PHONY: all clean test bootstrap bootstrap-selfhost

all: tavm $(DYNAMIC_MODS)

tavm: $(OBJ)
	$(CC) $(CFLAGS) $(RDYNAMIC) -o $@ $(OBJ) -lpthread $(LDLIBS)

%.o: %.c ta.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Shared library for dynamic C module loading
# macOS needs -undefined dynamic_lookup; Linux allows undefined symbols by default
lib/%.so: lib/%.c ta.h
	$(CC) $(CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< -lpthread $(LDLIBS)

lib/%.dylib: lib/%.c ta.h
	$(CC) $(CFLAGS) -fPIC -shared $(UNDEF_OK) -o $@ $< -lpthread $(LDLIBS)

clean:
	rm -f $(OBJ) tavm $(DYNAMIC_MODS)

# Test suite uses the tinyactor shell script + tavm binary
test: tavm tinyactor
	@echo "Running all tests..."
	@bash test/run_all_tests.sh

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
