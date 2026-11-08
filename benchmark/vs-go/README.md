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
| light request (64KB digest) | 0.073 ms | 0.053 ms | 1.38× |
| light req/s (one client, pure light) | 13250/s | 18391/s | 0.72× |
| heavy request (16MB digest) | 5.565 ms | 5.570 ms | **1.00×** |
| heavy req/s (one client, pure heavy) | 177/s | 177/s | **1.00×** |

Two things to read out of it:

* **The kernel costs the same on both sides** — the 16MB request costs
  5.565 ms on tinyactor and 5.570 ms on go; the 64KB one costs 0.073 ms and
  0.053 ms. The difference (5.49 ms against 5.52 ms) is the SHA-256 of the
  16MB minus the 64KB, i.e. ~3.0 GB/s on both sides. **The heavy request is
  a tie** — calling C from TA costs nothing over calling it from Go.
* **The request path is TA code** — HTTP head parsing, JSON out, the actor
  dispatch. The 64KB digest itself is ~0.022 ms at that rate, so the path
  around it costs ~0.051 ms on tinyactor against ~0.031 ms on go: that is
  the 1.38× on the light row. A round ago the same subtraction gave ~0.089
  against ~0.029 — the batch parsing primitives (`str.pairs`, `str.cmp`,
  `dict` batch build, PR #410) cut the TA request path by ~40%, and the
  remaining gap is still the runtime, not the kernel.

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
tinyactor faster. The three tables below were recorded on 2026-11-08 at commit
`f0090fb`, running the bytecode VM (`tinyactor run server.ta`); the last
section compares them with the 2026-10-21 round.

### One worker each

| | tinyactor | go | ta/go |
|---|---|---|---|
| **delay p50** | **11.32** | **28.26** | **0.40×** |
| **delay p99** | **17.67** | **61.69** | **0.29×** |
| light p50 | 9.92 | 16.95 | 0.59× |
| light p99 | 15.96 | 44.50 | 0.36× |
| heavy p50 | 54.40 | 22.48 | 2.42× |
| heavy p99 | 71.66 | 49.98 | 1.43× |
| delay req/s | 352 | 136 | 2.59× |
| light req/s | 672 | 687 | 0.98× |
| heavy req/s | 172 | 177 | 0.97× |
| total req/s | 1196 | 1000 | 1.20× |
| cores used | 1.01 | 1.00 |

The latency-sensitive endpoint answers **2.5× faster at p50 and 3.5× faster at
p99**, while the digest load runs at the same rate on both sides (0.98×) — and
tinyactor serves 20% more requests in total, because it gets through the
probes two and a half times as fast.

`heavy p50` is the row that got worse since the last round (37.3 → 54.4 ms),
and it is the same regression that pushed `delay p50` from 8.7 to 11.3 — see
[Two rounds on the same machine](#two-rounds-on-the-same-machine) for the
bisect. It reproduces in three independent runs (54.0 / 54.4 / 54.5) and also
without the probes, so it is not noise and not the probes' fault. `heavy p99`
is the noisy cell here: 70–86 ms across those three runs.

### Six workers each

Same load, six workers per side — the shape a real service runs in:

| | tinyactor | go | ta/go |
|---|---|---|---|
| **delay p50** | **2.27** | **8.77** | **0.26×** |
| **delay p99** | **4.64** | **26.28** | **0.18×** |
| light p50 | 1.46 | 0.56 | 2.62× |
| light p99 | 3.20 | 15.00 | 0.21× |
| heavy p50 | 13.58 | 7.98 | 1.70× |
| heavy p99 | 17.02 | 21.67 | 0.79× |
| delay req/s | 1679 | 390 | 4.31× |
| light req/s | 3268 | 3423 | 0.95× |
| heavy req/s | 813 | 851 | 0.96× |
| total req/s | 5760 | 4664 | 1.23× |
| cores used | 5.83 | 5.84 |

The small requests are the interesting row: **p99 3.20 ms against Go's
15.00 ms** — the CPU-bound traffic drags Go's tail up, and does not drag
tinyactor's nearly as much. The p50 is the other side of the same coin
(1.46 ms against 0.56 ms): tinyactor's slice is coarse, so a small request
sometimes waits for the current slice to end.

### Eight workers each

| | tinyactor | go | ta/go |
|---|---|---|---|
| **delay p50** | **1.91** | **8.44** | **0.23×** |
| **delay p99** | **6.71** | **24.41** | **0.27×** |
| light p50 | 1.03 | 0.45 | 2.26× |
| light p99 | 5.13 | 12.34 | 0.42× |
| heavy p50 | 12.04 | 8.40 | 1.43× |
| heavy p99 | 18.33 | 21.53 | 0.85× |
| delay req/s | 1930 | 439 | 4.40× |
| light req/s | 3808 | 4051 | 0.94× |
| heavy req/s | 950 | 1008 | 0.94× |
| total req/s | 6688 | 5497 | 1.22× |
| cores used | 7.10 | 7.17 |

Go's delay latency is pinned at "one timeslice" (~8.4 ms) however many Ps it
gets; tinyactor's does not get worse with more workers. Eight workers is also
where tinyactor's total throughput peaks in this run (6688/s against 5760/s at
six workers): the machine has 4 performance and 4 efficiency cores, so the
last two workers buy less than the first six.

### The same mix without the probes

Six workers, `DELAYC=0` — the digest mix alone:

| | tinyactor | go | ta/go |
|---|---|---|---|
| light p50 | 1.40 | 0.53 | 2.64× |
| light p99 | 2.75 | 15.17 | 0.18× |
| heavy p50 | 13.32 | 7.80 | 1.71× |
| heavy p99 | 16.21 | 21.60 | 0.75× |
| light req/s | 3363 | 3468 | 0.97× |
| heavy req/s | 838 | 863 | 0.97× |
| total req/s | 4201 | 4332 | 0.97× |
| cores used | 5.94 | 5.90 |

**A tie** (0.97×), with the same shape: tinyactor's median is worse, its tail
is better. The total-throughput win in the sched case comes from the
latency-sensitive part, not from the digest part.

### Where the mixed numbers come from

With one worker, the heavy requests are ~90% of the CPU (16MB of SHA-256
each), so the run is a study of **how each runtime hands that worker back**:

* The tinyactor VM charges a reduction per control transfer and returns the
  worker when the slice budget (1000) is spent. A 16MB digest is 256 chunks
  of 64KB, so a heavy request hands the worker back a handful of times — the
  profiler counted 7 budget exhaustions per heavy request in the 2026-10-21
  round.
* Go's goroutine is preemptible at every function call, so the 256 chunks of a
  heavy request interleave with the other connections' requests 256 times.

The two distributions put the wait in different places. On the tinyactor side
the light requests are served sooner (p50 9.9 ms, p99 16.0 ms, against
17.0/44.5), and the heavy ones later (p50 54.4 ms, against 22.5 ms). Same
trade as before, in the same direction: the tail of the small requests is what
the coarse scheduler buys, and the large requests pay for it. The light side
of that trade got better in the 2026-11-08 round and the heavy side got worse
— same commit, see below.

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
| 1 | 29 | 34466 | 1188× |
| 2 | 47 | 28763 | 612× |
| 4 | 52 | 26068 | 501× |
| 8 | 5091 | 32965 | 6.5× |

(The TA side is the module `benchmark/vs-go/fairness.ta`; the Go side is
`server.go`'s handler. The repository's own benchmark — category
actor/fairness in `benchmark/run_benchmarks.sh` — runs the same protocol
through the thin runner `benchmark/vs-go/fairness_run.ta`, so the protocol
has one implementation on the TA side. Over 2 runs the TA side's numbers
move within 29–29 / 15–47 / 39–52 / 5091–8449, the Go side's within
34466–36690 / 25401–28763 / 26068–34042 / 32965–44300.)

Both sides run a plain compute loop that the runtime has to preempt — neither
side yields voluntarily — so the difference is the scheduler, not the work.
The 8-worker row is the noisy one: 32 actors over 8 workers on a machine with
4 performance and 4 efficiency cores, and where a worker lands decides the
spread.

## Two rounds on the same machine

Same M3, same scripts, two points in time: `d3fe81c` (2026-10-21, the numbers
this file had until now) and `f0090fb` (2026-11-08). To make sure the older
numbers were not machine drift, `d3fe81c` was rebuilt in a worktree and the
one-worker sched case was re-run on the same afternoon as the new ones — it
reproduced the recorded values (heavy p50 37.29 → 37.29, delay p50 8.69 →
8.73, light p50 14.67 → 14.61). So the differences below are code, not the
machine.

| one worker, sched case | 2026-10-21 | 2026-11-08 |
|---|---|---|
| delay p50 | 8.69 | 11.32 |
| delay p99 | 16.99 | 17.67 |
| light p50 | 14.67 | **9.92** |
| light p99 | 29.01 | **15.96** |
| heavy p50 | 37.29 | **54.40** |
| heavy p99 | 60.47 | 71.66 |
| total req/s | 1259 | 1196 |

Two things moved, in opposite directions, and both are the same commit:
`9f25c4e` ("perf: 批量请求解析原语 — str.pairs / str.cmp / dict 批量构造",
PR #410). Bisected on this machine, one-worker sched case each:

| commit | light p50 | heavy p50 | delay p50 | total req/s |
|---|---|---|---|---|
| `d3fe81c` | 14.61 | 37.29 | 8.73 | 1252 |
| `c97146a` (just before #410) | 14.72 | 37.57 | 8.78 | 1252 |
| `9f25c4e` (#410 itself) | **9.94** | **54.53** | 11.46 | 1188 |
| `22e7924` (before the C-backend work) | 9.94 | 54.47 | 11.46 | 1188 |
| `f0090fb` (HEAD) | 9.92 | 54.40 | 11.32 | 1196 |

The C-backend commits (`f89ab24` onward) change nothing here: the same binary
path is the VM, and `22e7924` and HEAD are identical to two decimals. The
whole delta comes from #410, which bought ~40% off the TA request path
(per-request light 0.112 → 0.073 ms, from the table above) and cost the
one-worker medians of the *other* requests: heavy p50 +46%, delay p50 +30%.
That is a trade inside the scheduler, not a regression in the parse code: with
one worker nothing runs in parallel, so anything that makes a single slice
longer or the per-request yield pattern different shows up as wait on
everyone else's p50. The guess worth testing next is that #410 moved work into
fewer, longer uninterrupted runs (the `dict` batch build is one cfunction call,
which cannot yield mid-way) — that would raise exactly these two rows while
lowering light latency, which is the shape observed. Not verified; the bisect
above is.

At six and eight workers the same commit is only visible as a mild heavy p50
drift (12.02 → 13.58 and 11.57 → 12.04) and better light tails (6.33 → 3.20,
6.66 → 5.13), because there is a spare worker to run the heavy digest while
the others serve light requests.

## Files

| | file | role |
|---|---|---|
| service | `server.ta` | the tinyactor side: `lib/http.ta`, one actor per connection |
| service | `server.go` | the Go side: `net/http` + `encoding/json` + `crypto/sha256` |
| client | `bench.go` | draws the mix, buckets the latencies, samples the digests, runs the delay probes, computes the fairness deviation |
| protocol | `fairness.ta` | the fairness protocol on the TA side: 32 CPU-bound actors, a 3s window, the iteration counts |
| runner | `fairness_run.ta` | the standalone runner of that protocol — the repository's own actor/fairness benchmark |
| build | `Makefile` | builds `go-server` and `bench` |
| runner | `run.sh` | both sides, one after the other, plus the table (`DELAYC` adds the probes, `WORKERS` sets the workers per side) |
| runner | `run-fairness.sh` | the fairness case: both sides, the same service, another endpoint, plus the table |
| runner | `serverctl.sh` | shared by the two runners: start a server on a free port, wait for it, stop it |

The kernel is not in this directory: it is `lib/sha256.ta` in the standard
library, and the C behind it (`lib/sha256.c`) is a lazy dylib the VM dlopens
on the first `sha256.*` reference.

`go-server`, `bench` and `fairness` are build output and are not in git.