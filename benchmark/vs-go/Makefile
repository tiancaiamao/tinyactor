# The benchmark's Go side: the server (cgo + OpenSSL) and the client.
#
#   make            # builds go-server and bench
#   ./go-server 8099
#   ./bench -url http://127.0.0.1:8099
#
# The cgo flags point at the same OpenSSL the TinyActor side links into
# lib/sha256.dylib — one kernel on both sides, so a difference in the
# numbers is a difference in the runtime and not in the workload.

UNAME := $(shell uname)
ifeq ($(UNAME),Darwin)
OPENSSL_PREFIX := $(shell brew --prefix openssl@3 2>/dev/null)
endif
ifneq ($(OPENSSL_PREFIX),)
OPENSSL_CFLAGS := -I$(OPENSSL_PREFIX)/include
OPENSSL_LIBDIR := $(OPENSSL_PREFIX)/lib
else
OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
OPENSSL_LIBDIR := $(shell pkg-config --libs-only-L openssl 2>/dev/null | sed 's/-L//g')
endif
CGO_CFLAGS := $(OPENSSL_CFLAGS)
ifneq ($(OPENSSL_LIBDIR),)
CGO_LDFLAGS := -L$(OPENSSL_LIBDIR) -Wl,-rpath,$(OPENSSL_LIBDIR) -lcrypto
else
CGO_LDFLAGS := -lcrypto
endif

GO ?= go

all: go-server bench

go-server: server.go
	CGO_CFLAGS="$(CGO_CFLAGS)" CGO_LDFLAGS="$(CGO_LDFLAGS)" $(GO) build -o $@ server.go

bench: bench.go
	$(GO) build -o $@ bench.go

clean:
	rm -f go-server bench