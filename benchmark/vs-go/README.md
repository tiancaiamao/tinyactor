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
* the Go side reaches **the same libcrypto** through cgo.

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
| light p50 | 13.3 | 16.8 | **0.79×** |
| light p99 | 23.1 | 43.9 | **0.53×** |
| heavy p50 | 47.8 | 22.2 | 2.16× |
| heavy p99 | 68.0 | 49.3 | 1.38× |
| total req/s | 784 | 863 | 0.91× |

Same work, same one worker, ~9% apart in throughput.

### What the request actually costs

One client at a time, no queueing — the per-request overhead of each runtime:

| | tinyactor | go |
|---|---|---|
| light request (64KB digest) | 0.122 ms | 0.122 ms |
| heavy request (16MB digest) | 5.84 ms | 5.70 ms |
| light req/s (one client) | 8020 | 8092 |

The HTTP + JSON + digest path costs the same on both sides. The 2.4% on the
heavy request is the FFI and the TA loop around the same 1024 native calls.

### Where the mixed numbers come from

With one worker, the heavy requests are ~90% of the CPU (16MB of SHA-256 each),
so the run is a study of **how each runtime shares that worker**:

* Go's cgo call releases the P, so the runtime can switch at every 16KB block.
* The tinyactor VM charges a reduction per control transfer and hands the
  worker back when the slice budget runs out — a few hundred native calls
  here, so the switch is coarser.

The two distributions show the trade that granularity buys. The heavy request
waits longer for its turn on the tinyactor side (47.8 ms of wall time for
5.8 ms of CPU, against 22.2 ms on Go), and the light requests queued behind it
are served sooner (light p99 23 ms against 44 ms).

Same trade as before, in the same direction: the tail of the small requests is
what the coarse scheduler buys, and the large requests pay for it.

## Files

| | file | role |
|---|---|---|
| service | `server.ta` | the tinyactor side: `lib/http.ta`, one actor per connection |
| service | `server.go` | the Go side: `net/http` + `encoding/json`, same routes |
| client | `bench.go` | draws the sizes, buckets the latencies, samples the digests |
| build | `Makefile` | builds `go-server` and `bench` (cgo flags for OpenSSL) |
| runner | `run.sh` | both sides, one after the other, plus the table |

The kernel is not in this directory: it is `lib/sha256.ta` in the standard
library, and the C behind it (`lib/sha256.c`) is a lazy dylib the VM dlopens
on the first `sha256.*` reference.

`go-server` and `bench` are build output and are not in git.