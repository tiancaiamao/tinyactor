# The benchmark's Go side: the server and the client.
#
#   make            # builds go-server and bench
#   ./go-server 8099
#   ./bench -url http://127.0.0.1:8099
#
# The Go server is plain Go — crypto/sha256 from the standard library, no
# cgo. The TinyActor side reaches OpenSSL's EVP through the standard
# library module lib/sha256.ta. Each side runs its own language's SHA-256,
# which is the comparison a real service would have.
#
# The server has all four endpoints of the service — the digest ones, the
# latency-sensitive one and the fairness one — so the client picks the
# load shape (bench.go) and the server only offers the work.

GO ?= go

all: go-server bench

go-server: server.go
	$(GO) build -o $@ server.go

bench: bench.go
	$(GO) build -o $@ bench.go

clean:
	rm -f go-server bench