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
do. The two implementations cost the same (see the per-request table below),
so the numbers compare the runtimes and not the kernels.

Both sides hash the same synthetic document (a 64-byte seed repeated), in the
same 64KB chunks, and both answer with the digest — so the runner can compare
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
`WORKERS=n` (in `run.sh`) gives each side n workers — `NWORKERS` on the
tinyactor side, `GOMAXPROCS` on the Go side.

## The numbers

M3, c=16, 12s, 80/20, one worker each (`NWORKERS=1` / `GOMAXPROCS=1`).
Latency in ms; ratio < 1 = tinyactor faster.

| | tinyactor | go | ta/go |
|---|---|---|---|
| light p50 | 15.4 | 17.0 | **0.90×** |
| light p99 | 31.9 | 44.5 | **0.72×** |
| heavy p50 | 36.3 | 22.5 | 1.62× |
| heavy p99 | 55.9 | 50.0 | 1.12× |
| total req/s | 804 | 865 | 0.93× |

Same work, same one worker, 7% apart in throughput — and the small requests
are where tinyactor is ahead.

### More workers

`WORKERS=4` / `WORKERS=8` on the tinyactor side, `GOMAXPROCS` on the Go side
(both sides, same load, 12s each):

| workers | tinyactor req/s | go req/s | ta/go | ta cores | go cores |
|---|---|---|---|---|---|
| 1 | 804 | 865 | 0.93× | 1.01 | 1.00 |
| 4 | 3027 | 3202 | 0.95× | 4.04 | 4.00 |
| 8 | 4213 | 4656 | 0.90× | 6.53 | 6.58 |

Both sides scale the same way; what is left between them is the request path,
not the scheduler.

### What the request actually costs

One client at a time, no queueing — the per-request cost of each runtime:

| | tinyactor | go |
|---|---|---|
| light request (64KB digest) | 0.118 ms | 0.053 ms |
| heavy request (16MB digest) | 5.65 ms | 5.60 ms |
| light req/s (one client, pure light) | 8347/s | 18102/s |

The kernel is the same on both sides. A 1KB request costs 0.097 ms on
tinyactor and 0.028 ms on go; the same request with 64KB costs 0.118 ms and
0.053 ms — the extra 0.021 ms and 0.025 ms is the SHA-256 of the 63KB more,
i.e. ~3 GB/s on both sides.

What is left over is the request path, and all of it is TA code: HTTP head
parsing, JSON in and out, the actor dispatch. 0.097 ms against Go's 0.028 ms
— that is the whole 2.2× on the light path. It is the runtime being compared,
not the kernel.

### Where the mixed numbers come from

With one worker, the heavy requests are ~90% of the CPU (16MB of SHA-256 each),
so the run is a study of **how each runtime hands that worker back**:

* The tinyactor VM charges a reduction per control transfer and returns the
  worker when the slice budget (1000) is spent. A 16MB digest is 256 chunks
  of 64KB, so a heavy request hands the worker back about 7 times (the VM
  profiler counts the budget exhaustions: 7 per request with 64KB chunks, 28
  with 16KB chunks).
* Go's goroutine is preemptible at every function call, so the 256 chunks of
  a heavy request interleave with the other connections' requests 256 times.

The two distributions put the wait in different places. On the tinyactor side
the light requests are served sooner (p50 15.4 ms, p99 31.9 ms, against
17.0/44.5), and the heavy ones later (p50 36.3 ms, against 22.5 ms). Same
trade as before, in the same direction: the tail of the small requests is what
the coarse scheduler buys, and the large requests pay for it.

### The chunk size is a knob

64KB is the chunk both sides hash in. It sets two things at once: how often
the FFI is crossed (one call per chunk on the tinyactor side) and how often a
heavy request hands the worker back. Measured, same load, 8 workers:

| chunk | ta/go throughput | ta heavy p50 | yields per 16MB request |
|---|---|---|---|
| 16KB | 0.86× | 13.1 ms | 28 |
| 64KB | 0.93× | 10.4 ms | 7 |

16KB yields four times more often and costs 4% of the throughput; 64KB keeps
the yields (7 per heavy request) and the chunking still shows up in the tail.

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