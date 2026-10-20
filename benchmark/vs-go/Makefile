# The benchmark's native module. `make` once, then:
#   tinyactor run server.ta 8099      # the VM dlopens lib/work.dylib
#   go run server.go 8099              # cgo links lib/libwork.dylib
UNAME := $(shell uname)
ifeq ($(UNAME),Darwin)
EXT := dylib
UNDEF := -undefined dynamic_lookup
else
EXT := so
UNDEF :=
endif

CC ?= cc
CFLAGS := -Wall -Wextra -std=c99 -O2 -D_DEFAULT_SOURCE -fPIC

all: lib/work.$(EXT) lib/libwork.$(EXT)

lib/work.$(EXT): work.c ../../ta.h
	@mkdir -p lib
	$(CC) $(CFLAGS) -shared $(UNDEF) -I../.. -o $@ work.c

# cgo's -lwork wants libwork.*; the VM wants work.*
lib/libwork.$(EXT): lib/work.$(EXT)
	cp $< $@

clean:
	rm -rf lib