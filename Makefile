CC      = cc
CFLAGS  = -Wall -Wextra -std=c99 -O2 -I.
SRC     = src/val.c src/reader.c src/compile.c src/vm.c src/api.c src/main.c
OBJ     = $(SRC:.c=.o)

tinyactor: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

%.o: %.c ta.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) tinyactor

.PHONY: clean