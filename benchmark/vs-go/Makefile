# The benchmark's Go side: the server and the client.
#
#   make            # builds go-server, bench, fairness
#   ./go-server 8099
#   ./bench -url http://127.0.0.1:8099
#
# The Go server is plain Go — crypto/sha256 from the standard library, no
# cgo. The TinyActor side reaches OpenSSL's EVP through the standard
# library module lib/sha256.ta. Each side runs its own language's SHA-256,
# which is the comparison a real service would have.
#
# fairness is the scheduler's fairness case (see run-fairness.sh): its Go
# side is a plain compute loop that the Go runtime has to preempt, which
# is exactly what the TinyActor side does.

GO ?= go

all: go-server bench fairness

go-server: server.go
	$(GO) build -o $@ server.go

bench: bench.go
	$(GO) build -o $@ bench.go

fairness: fairness.go
	$(GO) build -o $@ fairness.go

clean:
	rm -f go-server bench fairness