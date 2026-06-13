# example/Makefile — Build examples for TinyActor VM

CC      = cc
CFLAGS  = -Wall -Wextra -std=c99 -O2 -I..
SRC     = $(wildcard ../src/*.c)
# Exclude main.c from object list — we provide our own main
OBJ     = $(filter-out ../src/main.o,$(SRC:.c=.o))

all: echo_server

echo_server: echo_server.c $(OBJ) ../ta.h
	$(CC) $(CFLAGS) -I.. -o $@ echo_server.c $(OBJ)

clean:
	rm -f echo_server

.PHONY: all clean