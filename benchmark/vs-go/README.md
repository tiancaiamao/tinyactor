# tinyactor vs Go

One service, one kind of request, and how much work a request does is a
parameter of that request:

```
GET /work/<n>   → n passes of work.c, then a small JSON document
```

The client draws n 80/20 (20000 light / 2000000 heavy) from a per-worker seeded
PRNG, so **both servers are handed the same sizes in the same order**. Nothing
about the load is tailored to either side.

What tinyactor gets to show is what happens *while* a heavy request runs: its
server splits n into 50-pass native calls, and the VM charges a reduction on
each call's control transfer, so a heavy request hands its worker back every
~2ms. Go's identical loop can only be one cgo-sized call, and a cgo call is
unschedulable until it returns.

| | file | role |
|---|---|---|
| workload | `work.c` / `work.ta` | the kernel, and the chunked loop over it |
| tinyactor | `server.ta` | `lib/http.ta`: one handler actor per connection |
| go | `server.go` | `net/http`, same routes, same kernel, same chunk |
| client | `bench.go` | draws the sizes, buckets the latencies |
| build | `workenv.sh` | builds the workload module into a workdir |
| runner | `run.sh` | `make benchmark-vs-go` |

## Run

```
make benchmark-vs-go              # CONC=16 DUR=12s HEAVYPCT=20
CONC=32 DUR=30s make benchmark-vs-go
bash benchmark/vs-go/run.sh       # same thing without make
```

Needs a `go` toolchain and a C compiler. Both servers are pinned to one worker
(`NWORKERS=1` / `GOMAXPROCS=1`) so what differs is scheduling policy and the
kernel, not core count; runs are sequential so they never fight for CPU.

## Results

M3, c=16, 12s, 80/20. Latency in ms; ratio < 1 = tinyactor faster.

| | tinyactor | go | ta/go |
|---|---|---|---|
| light p50 | 62.1 | 180.9 | 0.34× |
| light p99 | 80.1 | 364.5 | 0.22× |
| heavy p50 | 2911 | 11452 | 0.25× |
| total req/s | 29 | 7 | 4.14× |

The kernel alone is 3.9× (0.186s vs 0.722s for `/work/2000000`, one worker), so
most of the above is the kernel. The rest is scheduling: run the same 10M-pass
burn as ONE native call instead of 50-pass chunks and the actor stops handing
its worker back — a heavy request becomes an unschedulable black box, exactly
what the Go side has.

### Known issue — do not read these numbers as steady-state

[#389](https://github.com/tiancaiamao/tinyactor/issues/389): every C→TA
callback copies the whole value in `val_converge_copy` (82% of CPU under
load). Sustained load collapses 78.5k → 11.5k req/s with RSS 3MB → 736MB, and
it does not recover after idle. Go is flat at ~78k / 17MB on the same machine.
Every tinyactor number above comes from a <10s window — the flattering part of
that curve. Fixing #389 is worth more than any tuning here.

## Reading the numbers

`bench.go` reports per class: `n`, `p50`, `p90`, `p99`, `max`, `errs`. The
throughput line is `light/s`, `heavy/s`, `total/s` over the measured window.

A request's latency and its completion rate are one fact seen twice — a closed
loop with `c` workers does `c / latency` requests per second (Little's law) —
so the independent numbers are the latencies and the rates, and the ratio table
above is derived from them.

Keep-alive is on: the client reuses connections (one per worker). Dropping it
measures connection setup, and there tinyactor loses 1.8× — the accept path,
not the service path.