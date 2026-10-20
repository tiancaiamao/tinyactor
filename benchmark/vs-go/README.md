# tinyactor vs Go — the same digest service

One service, one kind of request, HTTP and JSON in and out:

```
POST /digest   {"bytes":N}   →   {"bytes":N,"sha256":"<hex>"}
```

The CPU work is SHA-256 over an N-byte document — a real library, not
benchmark code:

* the tinyactor side calls `sha256` from the standard library (`lib/sha256.ta`
  → `lib/sha256.c` → libcrypto). From the TA code it is a plain import; the C
  is not visible.
* the Go side calls `crypto/sha256` from the Go standard library — no cgo, no
  C in the picture at all.

Each side runs its own language's SHA-256, which is what a real service would
do. The two implementations turn out to cost the same (see the per-request
table below), so the numbers compare the runtimes and not the kernels.

Both sides hash the same synthetic document (a 64-byte seed repeated), in the
same 16KB blocks, and both answer with the digest — so the runner can compare
the two services' responses byte for byte. Same work, or the comparison is
worthless.

The client draws the sizes 80/20 from a per-worker seeded PRNG, so **both
servers are handed the same sizes in the same order**.

## Run it

```sh
cd benchmark/vs-go
make                              # builds the Go server and the client, once

tinyactor run server.ta 8099      # the tinyactor server
./go-server 8099                  # the go server
./bench -url http://127.0.0.1:8099
```

`make benchmark-vs-go` at the repo root runs both sides and prints the table.

## The numbers

M3, c=16, 12s, 80/20, one worker each (`NWORKERS=1` / `GOMAXPROCS=1`).
Latency in ms; ratio < 1 = tinyactor faster.

| | tinyactor | go | ta/go |
|---|---|---|---|
| light p50 | 13.3 | 17.4 | **0.76×** |
| light p99 | 23.3 | 44.4 | **0.52×** |
| heavy p50 | 47.4 | 22.8 | 2.08× |
| heavy p99 | 70.0 | 50.0 | 1.40× |
| total req/s | 778 | 841 | 0.93× |

Same work, same one worker, ~7% apart in throughput.

### What the request actually costs

One client at a time, no queueing — the per-request cost of each runtime
(`./bench -c 1`):

| | tinyactor | go |
|---|---|---|
| light request (64KB digest) | 0.119 ms | 0.053 ms |
| heavy request (16MB digest) | 5.64 ms | 5.57 ms |
| light req/s (one client, pure light) | 8193/s | 18179/s |

Two things to read out of it:

* **The heavy path costs the same on both sides** (5.64 vs 5.57 ms) — OpenSSL's
  EVP and the Go standard library's SHA-256 are equally fast here, so the
  heavy numbers below are about scheduling, not about the kernel.
* **The light path is 2.2× dearer on tinyactor** (0.119 vs 0.053 ms): the
  request path is TA code — HTTP head parsing, JSON in and out, the actor
  dispatch — and the digest crosses the FFI four times for a 64KB document.

### Where the mixed numbers come from

With one worker, the heavy requests are ~90% of the CPU (16MB of SHA-256 each),
so the run is a study of **how each runtime hands that worker back**:

* The tinyactor VM charges a reduction per control transfer and returns the
  worker when the slice budget (1000) is spent. A 16MB digest is 1024 blocks,
  each one a TA call plus a native call — so the worker comes back two or
  three times during a heavy request.
* Go's goroutine is preemptible at every function call, so the 1024 blocks of
  a heavy request interleave with the other connections' requests 1024 times.

The two distributions put the wait in different places. On the tinyactor side
the light requests are served sooner (p50 13.3 ms, p99 23.3 ms, against
17.4/44.4), and the heavy ones later (p50 47.4 ms, against 22.8 ms). Same
trade as before, in the same direction: the tail of the small requests is what
the coarse scheduler buys, and the large requests pay for it.

## Files

| | file | role |
|---|---|---|
| service | `server.ta` | the tinyactor side: `lib/http.ta`, one actor per connection |
| service | `server.go` | the Go side: `net/http` + `encoding/json` + `crypto/sha256` |
| client | `bench.go` | draws the sizes, buckets the latencies, samples the digests |
| build | `Makefile` | builds `go-server` and `bench` |
| runner | `run.sh` | both sides, one after the other, plus the table |

The kernel is not in this directory: it is `lib/sha256.ta` in the standard
library, and the C behind it (`lib/sha256.c`) is a lazy dylib the VM dlopens
on the first `sha256.*` reference.

`go-server` and `bench` are build output and are not in git.