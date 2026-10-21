# tinyactor vs Go — one service, four endpoints, the client picks the load

```
POST /light     (no body)   →   {"bytes":65536,"sha256":"<hex>"}
POST /heavy     (no body)   →   {"bytes":16777216,"sha256":"<hex>"}
POST /delay1ms  (no body)   →   {"delay_ms":1}
POST /fairness  (no body)   →   {"counts":[c0,...,c31]}
```

One service, no parameters: the workload is part of the protocol, so both
sides do exactly the same work. The **client** decides the load shape — how
much of it is CPU-bound (`/light` vs `/heavy`), how many latency-sensitive
probes run alongside (`/delay1ms`), or the scheduler's fairness case
(`/fairness`).

`/light` and `/heavy` are the CPU-bound work: SHA-256 over a synthetic
document — a real library, not benchmark code:

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

`/fairness` is the scheduler's fairness case: the service spawns 32
CPU-bound workers, each spinning until a 3s deadline, and answers with the
per-worker iteration counts. The service does the work; the client computes
the deviation from the counts, so both sides are measured the same way.

Both sides hash the same synthetic document (a 64-byte seed repeated), in the
same 64KB blocks, and both answer with the digest — so the runner can compare
the two services' responses byte for byte. Same work, or the comparison is
worthless.

The client draws the mix from a per-worker seeded PRNG, so **both servers are
handed the same endpoints in the same order**.

> The digest state has to be a value the actor owns, not a handle into a
> global table — otherwise the standard library's own lock serializes the
> workers and the benchmark measures that lock instead of the runtime. That is
> PR #398 (`lib/sha256.ta`); this directory builds on top of it.

## Run it

```sh
cd benchmark/vs-go
make                              # builds the Go server and the client, once

tinyactor run server.ta 8099      # the tinyactor server
./go-server 8099                  # the go server
./bench -url http://127.0.0.1:8099
```

`make benchmark-vs-go` at the repo root runs both sides and prints the table.
`make benchmark-vs-go-fairness` runs the fairness case (the same service,
another endpoint — see below).

`run.sh` takes two knobs:

| knob | default | what it does |
|---|---|---|
| `WORKERS` | 1 | workers per side: `NWORKERS` on the tinyactor side, `GOMAXPROCS` on the Go side |
| `DELAYC` | 0 | latency-sensitive probes against `POST /delay1ms`, each on its own connection. 4 is the sched case |
| `HEAVYPCT` | 20 | share of the request mix that goes to `/heavy` (the rest goes to `/light`) |
| `CONC`, `DUR` | 16, 12s | client concurrency and measured window |

```sh
cd benchmark/vs-go
./run.sh                    # the digest mix, one worker each
DELAYC=4 ./run.sh           # the sched case, one worker each
WORKERS=6 DELAYC=4 ./run.sh # the sched case, six workers each
```

## What the request actually costs

One client at a time, no queueing — the per-request cost of each runtime
(`./bench -c 1`):

| | tinyactor | go | ta/go |
|---|---|---|---|
| light request (64KB digest) | 0.112 ms | 0.052 ms | 2.15× |
| light req/s (one client, pure light) | 8435/s | 18553/s | 0.45× |
| heavy request (16MB digest) | 6.091 ms | 6.052 ms | **1.01×** |
| heavy req/s (one client, pure heavy) | 163/s | 158/s | **1.03×** |

Two things to read out of it:

* **The kernel costs the same on both sides** — the 16MB request costs
  6.091 ms on tinyactor and 6.052 ms on go; the 64KB one costs 0.112 ms and
  0.052 ms. The difference (5.98 ms against 6.00 ms) is the SHA-256 of the
  16MB minus the 64KB, i.e. ~2.7 GB/s on both sides. **The heavy request is
  a tie** — calling C from TA costs nothing over calling it from Go.
* **The request path is TA code** — HTTP head parsing, JSON out, the actor
  dispatch. Subtracting the kernel leaves ~0.089 ms on tinyactor against
  ~0.029 ms on go: that is the whole 2.15× on the light path. It is the
  runtime being compared, not the kernel, and it is the next thing to
  optimize (`lib/http.ta` / `lib/json.ta`).

## The sched case — a latency-sensitive request under a CPU-dense load

This is the case [tiancaiamao/sched](https://github.com/tiancaiamao/sched)
makes: a service where one request is latency-sensitive and the others burn
the CPU. The demo hits `/delay1ms` while the checksum work saturates the
machine, and on Go the delay request's latency is dragged up by the CPU-bound
work — Go's runtime has no priority scheduler, so a woken goroutine waits
behind whatever is running.

Here the same shape is built out of the service above: the 80/20 digest load
is the CPU-dense part, `POST /delay1ms` is the latency-sensitive part, and the
client runs both against the same server at the same time.

The probes get their own connections and their own pool, and they are **not**
part of the request mix: a probe that is slow must not take capacity away from
the load it measures, or the two sides would not be doing the same work. The
digest load itself is identical on both sides — same endpoints, same order,
and the runner checks that the two services answered with the same bytes
(`same work: yes`).

M3 8 cores, c=16, 12s, 80/20, 4 probes. Latency in ms; ratio < 1 =
tinyactor faster.

### One worker each

| | tinyactor | go | ta/go |
|---|---|---|---|
| **delay p50** | **8.69** | **28.63** | **0.30×** |
| **delay p99** | **16.99** | **66.92** | **0.25×** |
| light p50 | 14.67 | 17.19 | 0.85× |
| light p99 | 29.01 | 45.44 | 0.64× |
| heavy p50 | 37.29 | 22.82 | 1.63× |
| heavy p99 | 60.47 | 52.20 | 1.16× |
| delay req/s | 444 | 135 | 3.29× |
| light req/s | 649 | 675 | 0.96× |
| heavy req/s | 166 | 173 | 0.96× |
| total req/s | 1259 | 983 | 1.28× |
| cores used | 1.01 | 0.92 |

The latency-sensitive endpoint answers **3.3× faster at p50 and 4.0× faster at
p99**, while the digest load runs at the same rate on both sides (0.96×) — and
tinyactor serves 28% more requests in total, because it gets through the
probes three and a half times as fast.

### Six workers each

Same load, six workers per side — the shape a real service runs in:

| | tinyactor | go | ta/go |
|---|---|---|---|
| **delay p50** | **2.33** | **8.94** | **0.26×** |
| **delay p99** | **6.15** | **25.27** | **0.24×** |
| light p50 | 2.38 | 1.07 | 2.22× |
| light p99 | 6.33 | 14.33 | 0.44× |
| heavy p50 | 12.02 | 8.79 | 1.37× |
| heavy p99 | 18.00 | 22.98 | 0.78× |
| delay req/s | 1596 | 396 | 4.03× |
| light req/s | 2839 | 3077 | 0.92× |
| heavy req/s | 710 | 767 | 0.93× |
| total req/s | 5146 | 4240 | 1.21× |
| cores used | 5.29 | 5.24 |

The small requests are the interesting row: **p99 6.33 ms against Go's
14.33 ms** — the CPU-bound traffic drags Go's tail up, and does not drag
tinyactor's nearly as much. The p50 is the other side of the same coin
(2.38 ms against 1.07 ms): tinyactor's slice is coarse, so a small request
sometimes waits for the current slice to end.

### Eight workers each

| | tinyactor | go | ta/go |
|---|---|---|---|
| **delay p50** | **1.98** | **7.70** | **0.26×** |
| **delay p99** | **7.30** | **25.58** | **0.29×** |
| light p50 | 1.88 | 0.89 | 2.12× |
| light p99 | 6.66 | 13.05 | 0.51× |
| heavy p50 | 11.57 | 9.47 | 1.22× |
| heavy p99 | 21.18 | 26.86 | 0.79× |
| delay req/s | 1779 | 473 | 3.76× |
| light req/s | 3099 | 3375 | 0.92× |
| heavy req/s | 773 | 841 | 0.92× |
| total req/s | 5651 | 4689 | 1.21× |
| cores used | 5.85 | 5.43 |

Go's delay latency is pinned at "one timeslice" (~9 ms) however many Ps it
gets; tinyactor's does not get worse with more workers.

### The same mix without the probes

Six workers, `DELAYC=0` — the digest mix alone:

| | tinyactor | go | ta/go |
|---|---|---|---|
| light p50 | 2.25 | 1.06 | 2.12× |
| light p99 | 6.28 | 14.98 | 0.42× |
| heavy p50 | 11.50 | 8.88 | 1.30× |
| heavy p99 | 16.81 | 24.44 | 0.69× |
| light req/s | 2974 | 3054 | 0.97× |
| heavy req/s | 743 | 762 | 0.98× |
| total req/s | 3717 | 3816 | 0.97× |
| cores used | 5.41 | 5.23 |

**A tie** (0.97×), with the same shape: tinyactor's median is worse, its tail
is better. The total-throughput win in the sched case comes from the
latency-sensitive part, not from the digest part.

### Where the mixed numbers come from

With one worker, the heavy requests are ~90% of the CPU (16MB of SHA-256
each), so the run is a study of **how each runtime hands that worker back**:

* The tinyactor VM charges a reduction per control transfer and returns the
  worker when the slice budget (1000) is spent. A 16MB digest is 256 chunks
  of 64KB, so a heavy request hands the worker back about 7 times (the VM
  profiler counts the budget exhaustions: 7 per request with 64KB chunks).
* Go's goroutine is preemptible at every function call, so the 256 chunks of a
  heavy request interleave with the other connections' requests 256 times.

The two distributions put the wait in different places. On the tinyactor side
the light requests are served sooner (p50 14.7 ms, p99 29.0 ms, against
17.2/45.4), and the heavy ones later (p50 37.3 ms, against 22.8 ms). Same
trade as before, in the same direction: the tail of the small requests is what
the coarse scheduler buys, and the large requests pay for it.

## The fairness case

The same service, another endpoint: Go's runtime is "barely FIFO" — it
hands a P to the goroutine that has been running longest, so one goroutine
can take a large share of a core while the others wait. `POST /fairness`
runs the protocol on both sides: 32 CPU-bound workers, each spinning for 3
seconds, and the answer carries the per-worker iteration counts. The
client computes the deviation.

```sh
cd benchmark/vs-go
./run-fairness.sh             # or: make benchmark-vs-go-fairness
```

`dev_x100000 = (max-min)/max * 100000` over the per-worker counts — the
smaller the better. One run of each:

| workers | tinyactor dev | go dev | ta/go |
|---|---|---|---|
| 1 | 14 | 33007 | 2358× |
| 2 | 7 | 21659 | 3094× |
| 4 | 69 | 35147 | 509× |
| 8 | 4997 | 35105 | 7.0× |

(The TA side is the module `benchmark/vs-go/fairness.ta`; the Go side is
`server.go`'s handler. The repository's own benchmark — category
actor/fairness in `benchmark/run_benchmarks.sh` — runs the same protocol
through the thin runner `benchmark/vs-go/fairness_run.ta`, so the protocol
has one implementation on the TA side. Over 3 runs the TA side's numbers
move within 14–16 / 7–39 / 8–277 / 4163–4997, the Go side's within
33007–42387 / 21659–28828 / 28583–35147 / 28005–35105.)

Both sides run a plain compute loop that the runtime has to preempt — neither
side yields voluntarily — so the difference is the scheduler, not the work.
The 8-worker row is the noisy one: 32 actors over 8 workers on a machine with
4 performance and 4 efficiency cores, and where a worker lands decides the
spread.

## Files

| | file | role |
|---|---|---|
| service | `server.ta` | the tinyactor side: `lib/http.ta`, one actor per connection |
| service | `server.go` | the Go side: `net/http` + `encoding/json` + `crypto/sha256` |
| client | `bench.go` | draws the mix, buckets the latencies, samples the digests, runs the delay probes, computes the fairness deviation |
| protocol | `fairness.ta` | the fairness protocol on the TA side: 32 CPU-bound actors, a 3s window, the iteration counts |
| runner | `fairness_run.ta` | the standalone runner of that protocol — the repository's own actor/fairness benchmark |
| fairness | `fairness.go` | the Go side of `benchmark/actor/fairness.ta` |
| build | `Makefile` | builds `go-server` and `bench` |
| runner | `run.sh` | both sides, one after the other, plus the table (`DELAYC` adds the probes, `WORKERS` sets the workers per side) |
| runner | `run-fairness.sh` | the fairness case: both sides, the same service, another endpoint, plus the table |
| runner | `serverctl.sh` | shared by the two runners: start a server on a free port, wait for it, stop it |

The kernel is not in this directory: it is `lib/sha256.ta` in the standard
library, and the C behind it (`lib/sha256.c`) is a lazy dylib the VM dlopens
on the first `sha256.*` reference.

`go-server`, `bench` and `fairness` are build output and are not in git.