CC      = cc
CFLAGS  = -Wall -Wextra -std=c99 -O2 -I.
SRC     = src/val.c src/reader.c src/reader_ta.c src/compile.c src/vm.c src/gc.c src/api.c src/module.c src/net.c src/http.c src/file.c src/buf.c src/str.c src/main.c
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

bootstrap: tinyactor
	./tinyactor lib/driver.ta --emit-tabc
	cp lib/driver.tabc lib/bootstrap.tabc
	@echo "wrote lib/bootstrap.tabc"

bootstrap-selfhost: bootstrap
	./tinyactor --bootstrap-emit lib/driver.ta lib/bootstrap_selfhost.tabc
	@echo "wrote lib/bootstrap_selfhost.tabc"

.PHONY: clean test bootstrap bootstrap-selfhost