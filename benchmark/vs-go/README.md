# tinyactor vs Go

One service, one kind of request, and how much work a request does is a
parameter of that request:

```
GET /work/<n>   → n passes of the kernel, then {"sum":<checksum>}
```

The client draws n 80/20 (20000 light / 2000000 heavy) from a per-worker seeded
PRNG, so **both servers are handed the same sizes in the same order**.

## Run it

```sh
cd benchmark/vs-go
make                              # builds the native module, once

tinyactor run server.ta 8099      # the tinyactor server
go run server.go 8099             # the go server
go run bench.go -url http://127.0.0.1:8099
```

`make benchmark-vs-go` at the repo root runs all three and prints the table.

## The kernel is the same code on both sides

`work.c` holds the kernel. The tinyactor server calls it through the VM's
native-module interface; the Go server calls **the same compiled library**
through cgo. Same function, same array, same recurrence, same chunking, same
seeds.

That matters: the obvious version of this benchmark gives one side a
vectorized C loop and the other a hand-written Go loop, and then reports the
difference as a runtime difference. It is not one — it is C versus Go.

So the servers also report the checksum of the work they did. The runner sends
both servers the same fixed sizes and compares the responses:

```
same work              yes
```

## What the numbers show

M3, c=16, 12s, 80/20, one worker each (`NWORKERS=1` / `GOMAXPROCS=1`).
Latency in ms; ratio < 1 = tinyactor faster.

| | tinyactor | go | ta/go |
|---|---|---|---|
| light p50 | 61.1 | 165.6 | **0.37×** |
| light p99 | 71.1 | 373.0 | **0.19×** |
| heavy p50 | 2802 | 2099 | 1.34× |
| heavy p99 | 2855 | 3100 | 0.92× |
| total req/s | 28 | 29 | 0.97× |

Same work, same worker, same throughput — but the two runtimes split that
worker differently. The tinyactor server runs a heavy request in 50-pass
native calls, and the VM charges a reduction on each call's control transfer,
so the actor hands its worker back every ~2ms and the light requests queued
behind it get served. Go's cgo call is atomic for the goroutine, so the heavy
request keeps the worker to itself for longer.

The cost is on the other side of the trade: the heavy request itself takes
1.34× longer, because it now gets less of the worker. That is the fairness
trade, stated in both directions.

### Known issue — these are not steady-state numbers

[#389](https://github.com/tiancaiamao/tinyactor/issues/389): every C→TA
callback copies the whole value in `val_converge_copy` (82% of CPU under
load). Sustained load collapses 78.5k → 11.5k req/s with RSS 3MB → 736MB, and
it does not recover after idle. Go is flat at ~78k / 17MB on the same machine.
Every tinyactor number above comes from a <10s window — the flattering part of
that curve. Fixing #389 is worth more than any tuning here.

## Files

| | file | role |
|---|---|---|
| kernel | `work.c` | the C function both sides call |
| tinyactor | `server.ta` | `lib/http.ta`, one handler actor per connection |
| go | `server.go` | `net/http`, same routes, same kernel via cgo |
| client | `bench.go` | draws the sizes, buckets the latencies |
| build | `Makefile` | builds `lib/work.dylib` (VM) and `lib/libwork.dylib` (cgo) |
| runner | `run.sh` | both sides, one after the other, plus the table |

`lib/` is build output and is not in git. The VM resolves a native module as
`lib/<mod>.<ext>` relative to the process CWD, which is why the server has to
be started from this directory.