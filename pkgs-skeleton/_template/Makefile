CC ?= cc
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
EXT := dylib
SHARED_FLAGS := -undefined dynamic_lookup
else
EXT := so
SHARED_FLAGS :=
endif

ROOT ?= ../..
OUT := template.$(EXT)

.PHONY: all clean test
all: $(OUT)

$(OUT): template.c $(ROOT)/ta.h
	$(CC) -Wall -Wextra -std=c99 -O2 -fPIC -shared $(SHARED_FLAGS) -I$(ROOT) -o $@ $<

clean:
		rm -f $(OUT)

# Requires a TinyActor checkout at ROOT and execution from its repository root.
test: all
	cp template.ta $(ROOT)/lib/template.ta
		cp $(OUT) $(ROOT)/lib/$(OUT)
	cd $(ROOT) && ./tinyactor run pkgs-skeleton/_template/smoke.ta