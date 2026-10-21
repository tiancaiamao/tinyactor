# tinyactor vs Go — the digest service, and the sched case

One service, two kinds of request, HTTP and JSON in and out:

```
POST /digest    {"bytes":N}   →   {"bytes":N,"sha256":"<hex>"}
POST /delay1ms  (no body)     →   {"delay_ms":1}
```

`/digest` is the CPU-bound work: SHA-256 over an N-byte document — a real
library, not benchmark code:

* the tinyactor side calls `sha256` from the standard library (`lib/sha256.ta`
  → `lib/sha256.c` → libcrypto). From the TA code it is a plain import; the C
  is not visible.
* the Go side calls `crypto/sha256` from the Go standard library — no cgo, no
  C in the picture at all.

Each side runs its own language's SHA-256, which is what a real service would
do. The two implementations turn out to cost the same (see the per-request
table below), so the numbers compare the runtimes and not the kernels.

`/delay1ms` is the latency-sensitive request: the server parks for a
millisecond and answers. Everything the client sees above that millisecond is
the runtime's wake-up latency — what
[tiancaiamao/sched](https://github.com/tiancaiamao/sched) is about, and what
this directory measures.

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
`make benchmark-vs-go-sched` runs it with the latency-sensitive probes on,
`make benchmark-vs-go-fairness` runs the fairness case.

## The sched case — a latency-sensitive request under a CPU-dense load

This is the case [tiancaiamao/sched](https://github.com/tiancaiamao/sched)
makes: a service where one request is latency-sensitive and the others burn
the CPU. The demo hits `/delay1ms` while `/checksum*` saturates the machine,
and on Go the delay request's latency is dragged up by the CPU-bound work —
Go's runtime has no priority scheduler, so a woken goroutine waits behind
whatever is running.

Here the same shape is built out of the service above: the 80/20 digest load
is the CPU-dense part, `POST /delay1ms` is the latency-sensitive part, and the
client runs both against the same server at the same time.

```sh
cd benchmark/vs-go
DELAYC=4 ./run.sh         # 4 latency-sensitive probes, same mixed load
WORKERS=6 DELAYC=4 ./run.sh   # 6 workers each (NWORKERS / GOMAXPROCS)
```

The probes get their own connections and their own pool, and they are **not**
part of the request mix: a probe that is slow must not take capacity away from
the load it measures, or the two sides would not be doing the same work. The
digest load itself is identical on both sides — same sizes, same order, and
the runner checks that the two services answered with the same bytes.

M3, c=16, 12s, 80/20, 4 probes, one worker each (`NWORKERS=1` /
`GOMAXPROCS=1`). Latency in ms; ratio < 1 = tinyactor faster.

| | tinyactor | go | ta/go |
|---|---|---|---|
| **delay p50** | **8.06** | **33.40** | **0.24×** |
| **delay p99** | **14.04** | **73.26** | **0.19×** |
| light p50 | 15.24 | 19.35 | 0.79× |
| light p99 | 28.47 | 53.79 | 0.53× |
| heavy p50 | 55.02 | 24.48 | 2.25× |
| heavy p99 | 80.55 | 60.81 | 1.32× |
| delay req/s | 490 | 120 | 4.08× |
| light req/s | 536 | 599 | 0.89× |
| heavy req/s | 138 | 153 | 0.90× |
| total req/s | 1164 | 872 | 1.33× |

The latency-sensitive endpoint answers **4.1× faster at p50 and 5.2× faster at
p99**, while the digest load runs at the same rate on both sides (0.90×) —
and tinyactor serves 33% more requests in total, because it gets through the
probes four times as fast. Five runs of the same command: the delay p50 ratio
stayed between 0.22× and 0.26×, the delay p99 ratio between 0.17× and 0.19×.

### Why Go is slow here, and why tinyactor is not

The delay request does one millisecond of sleeping and almost no work. What
the client measures on top of that millisecond is how long the runtime takes
to run it again.

*On the Go side:* a goroutine that wakes from a timer needs a P to run on. The
P is busy with the digest requests, and Go's scheduler has no notion of a
latency-sensitive task — the sched repo calls the Go runtime "barely FIFO" —
so the woken goroutine waits behind the running one until the scheduler hands
the P over. The 1ms sleep becomes 33ms at p50 and 73ms at p99.

*On the tinyactor side:* the VM charges a reduction per control transfer and
hands the worker back when the slice budget (1000 reductions) is spent
(`src/vm.c:395`, `src/scheduler.c:587`). `time.sleep` parks the actor on the
poller's deadline, and the poller enqueues it again the moment that deadline
passes (`src/scheduler.c:740`); the worker that comes back to the run queue
picks it up. So the wait is one slice — tens of microseconds — not one
timeslice.

Same machine, same load, same client, same protocol: the difference is the
scheduler's idea of what deserves to run next.

### More workers — and the sha256 lock on this branch

One worker each is the clean comparison (one runtime thread against one Go P),
but a real service runs several workers, so here is the same case with six.

On this branch the digest load does not scale, and the reason is not the
scheduler: `lib/sha256.ta` keeps the digest state in a global slot table
behind a lock, so two workers hashing at once serialize against each other.
Go's `crypto/sha256` keeps the state in the goroutine's own stack — there is
no such table. #398 takes the global table and the lock out of `lib/sha256.ta`;
the right-hand columns below are the same run on that branch.

Same load, six workers each (c=16, 12s, 80/20), requests per second:

| | this branch: ta | this branch: go | #398: ta | #398: go |
|---|---|---|---|---|
| light | 525 | 3279 | 3120 | 3324 |
| heavy | 134 | 815 | 779 | 828 |
| total | 1305 | 4502 | 3899 | 4152 |

On this branch tinyactor serves a fifth of Go's requests — that is the lock,
not the runtime: with the lock gone it is at 0.94× of Go. The sched case on
this branch at six workers still wins the delay metric (delay p50 6.03 vs Go
8.66, p99 11.21 vs 25.98), but the lock eats most of the advantage: the
digest work serializes, so the machine is not actually saturated by
tinyactor.

The sched case, six workers each, on the #398 build (same work on both
sides):

| | tinyactor | go | ta/go |
|---|---|---|---|
| **delay p50** | **1.90** | **8.86** | **0.21×** |
| **delay p99** | **4.18** | **24.73** | **0.17×** |
| light p50 | 2.21 | 0.65 | 3.37× |
| light p99 | 5.50 | 14.58 | 0.38× |
| heavy p50 | 11.28 | 8.15 | 1.38× |
| heavy p99 | 21.31 | 22.41 | 0.95× |
| delay req/s | 2022 | 395 | 5.12× |
| light req/s | 3023 | 3382 | 0.89× |
| heavy req/s | 754 | 842 | 0.90× |
| total req/s | 5798 | 4619 | 1.26× |

Eight workers, same build: delay p50 1.56 vs 8.91 (0.17×), delay p99 4.08 vs
27.37 (0.15×), total 6571 vs 4947 (1.33×). Go's delay latency is one
timeslice (~10ms) once there are enough Ps to run the digest load in
parallel — it does not improve at eight — while tinyactor's does not get
worse.


### The fairness case

The second half of the sched story: Go's runtime is "barely FIFO" — it hands
a P to the goroutine that has been running longest, so one goroutine can take
a large share of a core while the others wait. The tinyactor side here is the
repository's own `benchmark/actor/fairness.ta`: 32 CPU-bound actors, each
spinning for 3 seconds, and the spread of their iteration counts is the
unfairness. `fairness.go` is the same protocol on the Go side.

```sh
cd benchmark/vs-go
./run-fairness.sh             # or: make benchmark-vs-go-fairness
```

`dev_x100000 = (max-min)/max * 100000` over the per-actor counts — the
smaller the better. Ranges over 5 runs:

| workers | tinyactor dev | go dev | ta/go |
|---|---|---|---|
| 1 | 14–15 | 34431–51667 | 2295–3690× |
| 2 | 7–23 | 45700–55367 | 1987–7910× |
| 4 | 11–1786 | 28790–48264 | 16–4388× |
| 8 | 4051–5551 | 32328–41482 | 6–10× |

(The ratio column is `go dev ÷ tinyactor dev`, taken at the two ends of the
ranges: the smallest Go deviation against the largest tinyactor one, up to the
largest Go deviation against the smallest tinyactor one.)

Both sides run a plain compute loop that the runtime has to preempt — neither
side yields voluntarily — so the difference is the scheduler, not the work.
The 8-worker row is the noisy one: 32 actors over 8 workers on a machine with
4 performance and 4 efficiency cores, and where a worker lands decides the
spread.

## The digest service

The same comparison without the probes — the service on its own, where the
two runtimes are at parity.

M3, c=16, 12s, 80/20, one worker each. Latency in ms; ratio < 1 = tinyactor
faster.

| | tinyactor | go | ta/go |
|---|---|---|---|
| light p50 | 13.3 | 17.0 | **0.78×** |
| light p99 | 23.5 | 44.7 | **0.53×** |
| heavy p50 | 47.5 | 22.5 | 2.11× |
| heavy p99 | 70.4 | 50.4 | 1.40× |
| total req/s | 778 | 861 | 0.90× |

Same work, same one worker, ~10% apart in throughput.

### What the request actually costs

One client at a time, no queueing — the per-request cost of each runtime
(`./bench -c 1`):

| | tinyactor | go |
|---|---|---|
| light request (64KB digest) | 0.131 ms | 0.052 ms |
| heavy request (16MB digest) | 5.69 ms | 5.60 ms |
| light req/s (one client, pure light) | 7457/s | 18462/s |

Two things to read out of it:

* **The heavy path costs the same on both sides** (5.69 vs 5.60 ms) — OpenSSL's
  EVP and the Go standard library's SHA-256 are equally fast here, so the
  heavy numbers below are about scheduling, not about the kernel.
* **The light path is 2.5× dearer on tinyactor** (0.131 vs 0.052 ms): the
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
the light requests are served sooner (p50 13.3 ms, p99 23.5 ms, against
17.0/44.7), and the heavy ones later (p50 47.5 ms, against 22.5 ms). Same
trade as before, in the same direction: the tail of the small requests is what
the coarse scheduler buys, and the large requests pay for it.

## Files

| | file | role |
|---|---|---|
| service | `server.ta` | the tinyactor side: `lib/http.ta`, one actor per connection |
| service | `server.go` | the Go side: `net/http` + `encoding/json` + `crypto/sha256` |
| client | `bench.go` | draws the sizes, buckets the latencies, samples the digests, runs the delay probes |
| fairness | `fairness.go` | the Go side of `benchmark/actor/fairness.ta` |
| build | `Makefile` | builds `go-server`, `bench` and `fairness` |
| runner | `run.sh` | both sides, one after the other, plus the table (`DELAYC` adds the probes, `WORKERS` sets the workers per side) |
| runner | `run-fairness.sh` | the fairness case, both sides, plus the table |

The kernel is not in this directory: it is `lib/sha256.ta` in the standard
library, and the C behind it (`lib/sha256.c`) is a lazy dylib the VM dlopens
on the first `sha256.*` reference.

`go-server`, `bench` and `fairness` are build output and are not in git.