CC      = cc
CFLAGS  = -Wall -Wextra -std=c99 -O2 -I.
SRC     = src/val.c src/reader.c src/reader_ta.c src/compile.c src/vm.c src/gc.c src/api.c src/net.c src/http.c src/file.c src/buf.c src/str.c src/main.c
OBJ     = $(SRC:.c=.o)

tinyactor: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) -lpthread

%.o: %.c ta.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) tinyactor

test: tinyactor
	@echo "Running all tests..."
	@bash test/run_all_tests.sh

# Bootstrap: use the TA compiler (inside existing bootstrap.tabc) to compile
# driver.ta into a new bootstrap.tabc. Requires an existing bootstrap.tabc
# on disk (committed in git, or generated via bootstrap-from-c).
bootstrap: tinyactor
	./tinyactor --bootstrap-emit lib/driver.ta lib/bootstrap.tabc
	@echo "wrote lib/bootstrap.tabc"

# Bootstrap from C: generate bootstrap.tabc using the C compiler directly.
# Needed only once when there's no existing bootstrap.tabc.
bootstrap-from-c: tinyactor
	./tinyactor --c-compile lib/driver.ta --emit-tabc
	cp lib/driver.tabc lib/bootstrap.tabc
	@echo "wrote lib/bootstrap.tabc (via C compiler)"

# Self-hosting: use TA compiler to emit bootstrap_selfhost.tabc,
# then verify it matches bootstrap.tabc (fixed point).
bootstrap-selfhost: bootstrap
	./tinyactor --bootstrap-emit lib/driver.ta lib/bootstrap_selfhost.tabc
	@echo "wrote lib/bootstrap_selfhost.tabc"
	@cmp lib/bootstrap.tabc lib/bootstrap_selfhost.tabc && echo "FIXED POINT VERIFIED" || echo "WARNING: fixed point mismatch!"

.PHONY: clean test bootstrap bootstrap-from-c bootstrap-selfhost