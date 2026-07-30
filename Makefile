CC      = cc
CFLAGS  = -Wall -Wextra -std=c99 -O2 -I.
SRC     = src/val.c src/vm.c src/gc.c src/api.c src/net.c src/http.c src/file.c src/buf.c src/str.c src/tavm.c
OBJ     = $(SRC:.c=.o)

tavm: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) -lpthread

%.o: %.c ta.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) tavm

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

.PHONY: clean test bootstrap bootstrap-selfhost