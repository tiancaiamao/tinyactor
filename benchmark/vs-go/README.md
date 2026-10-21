# tinyactor vs Go

Two suites, one rule: **the same protocol on both sides, doing the same
work.** A comparison that only wins because the other side was held back is
not a comparison.

| suite | what it measures | who wins |
|---|---|---|
| scheduler (`run-sched.sh`) | fairness under oversubscription, wake-up latency under CPU load | **tinyactor: 3–2400× fairer, 21× faster to wake up** |
| digest service (`run.sh`) | end-to-end HTTP + JSON + SHA-256 throughput and latency | parity (0.90–0.95×) |

## 1. The scheduler cases

```sh
cd benchmark/vs-go
make
./run-sched.sh              # both cases, both sides, one table
```

The scheduler suite is the interesting one. It is the case
[tiancaiamao/sched](https://github.com/tiancaiamao/sched) is about: Go has no
priority scheduler, so a latency-sensitive task waits behind CPU-bound ones
— and the patch that fixes it is a scheduler patch, not an application
patch. tinyactor's scheduler is built the other way round, and the numbers
below are the two of them, same protocol, side by side.

### Case 1 — fairness: 32 CPU-bound workers, 3 seconds

Each worker spins on the clock until a shared wall-clock deadline and counts
its iterations. The worker that gets more CPU does more iterations, so the
spread of the counts is the unfairness:

```
dev_x100000 = (max - min) / max * 100000        # 15 means 0.015%
```

The tinyactor side is `benchmark/actor/fairness.ta`; the Go side is
`fairness.go`, the same protocol call for call. **Neither side yields
voluntarily** — both are plain compute loops that the runtime has to
preempt, which is the whole point.

M3, 8 cores, `NWORKERS=n` on the tinyactor side, `GOMAXPROCS=n` on the Go
side. Ten runs each (five at `workers=1`); the range is what the runs
actually produced:

| workers | tinyactor dev | go dev | tinyactor is fairer by (worst case for it) |
|---|---|---|---|
| 1 | 14–15 (0.014–0.015%) | 35 763–43 047 (36–43%) | **2384×** |
| 2 | 7–30 (0.007–0.030%) | 39 376–46 927 (39–47%) | **1312×** |
| 4 | 11–1 764 (0.01–1.8%) | 26 230–45 794 (26–46%) | **15×** |
| 8 | 2 717–5 923 (2.7–5.9%) | 18 103–38 948 (18–39%) | **3.1×** |

The "fairer by" column divides the *best* Go run by the *worst* tinyactor
run, so it is the number to believe when the machine is busy: even then,
tinyactor hands out CPU time more evenly than Go does. The gap narrows as
the worker count approaches the core count — with one worker per core the
two runtimes are both spreading the same 32 actors over 8 CPUs, and Go's
worst run is still 3× less even than tinyactor's worst run.

### Case 2 — wake-up latency: a 1ms sleep under CPU load

One worker asks for a 1ms sleep while N CPU-bound workers fill the machine.
The delay it actually sees is the scheduler's wake-up latency: the sleep
itself is 1000µs, everything above that is the scheduler.

`latency.ta` and `latency.go`, same protocol, 8 workers on each side. Three
runs each:

| noise | ta p50 | ta p99 | ta wake-ups | go p50 | go p99 | go wake-ups |
|---|---|---|---|---|---|---|
| 2 | 1 135 µs | 1 158–1 160 µs | ~2 650 | 1 130 µs | 1 143–1 248 µs | ~2 650 |
| 4 | 1 167–1 169 µs | 3 315–3 781 µs | ~2 300 | 1 148–1 150 µs | 1 208–1 278 µs | ~2 600 |
| 8 | **1 169–1 183 µs** | **4 661–5 264 µs** | **~2 140** | **24 436–25 309 µs** | **37 824–51 428 µs** | **~115** |
| 16 | **1 566–1 586 µs** | **4 942–5 613 µs** | **~1 830** | **24 493–25 026 µs** | **37 333–44 217 µs** | **~115** |

(The column *wake-ups* is how many 1ms sleeps the probe actually completed in
the 3s window — 2 140 is "about one per millisecond", 115 is "one every
26ms".)

Read the first two rows before the last two: below saturation the two sides
are the same number, so the harness is not rigged. The cliff is at
`noise = cores`, where every Go P is running a CPU-bound goroutine — and then
Go's wake-up delay goes up by **21×** while tinyactor's does not move.

### Why the two sides differ

**tinyactor** hands the worker back on a budget, not on a timer. The
interpreter charges a reduction per control transfer and returns the actor
when the slice is spent (`src/vm.c:395`, `MAX_REDUCTIONS` in
`src/scheduler.c:587`); the actor is re-queued on the global FIFO runq
(`runq_enqueue` / `runq_trydequeue`, `src/scheduler.c:275`), so a task that
just woke up is next in line within a slice — a slice is ~1000 reductions,
microseconds, not milliseconds. A sleeping actor parks on the poller's
deadline (`lib/time.c`, the poll-based sleep) and no worker ever blocks.

**Go** has no priority scheduler, and its preemption points are coarse: a
CPU-bound goroutine keeps its P until the runtime's timeslice expires (10ms)
or it hits a preemption point. A goroutine whose timer just fired sits in
the runq of a P that is busy, so it waits for the timeslice to end — 24ms of
waiting in the numbers above. This is the weakness `tiancaiamao/sched` runs
into and patches in Go itself (golang/go#51347).

Neither side has priorities, and neither side has a priority scheduler to
cheat with. The difference is the *slice length*: tinyactor's is measured in
reductions, Go's in milliseconds.

### A note on the Go side

A Go loop that calls `runtime.Gosched()` every iteration is a different
workload — it yields on purpose, so the scheduler never has to preempt it.
Measured, that variant lands at ~4–6% deviation instead of ~40%. The
tinyactor side has no such knob (there is no yield builtin), so the
like-for-like protocol is the loop the runtime must preempt — which is what
both sides here run.

## 2. The digest service — same work, parity

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
| scheduler | `benchmark/actor/fairness.ta` | the tinyactor side of case 1 (kept with the other actor benchmarks) |
| scheduler | `fairness.go` | the Go side of case 1 |
| scheduler | `latency.ta` | the tinyactor side of case 2 |
| scheduler | `latency.go` | the Go side of case 2 |
| scheduler | `run-sched.sh` | both cases, both sides, one table |
| service | `server.ta` | the tinyactor side: `lib/http.ta`, one actor per connection |
| service | `server.go` | the Go side: `net/http` + `encoding/json` + `crypto/sha256` |
| client | `bench.go` | draws the sizes, buckets the latencies, samples the digests |
| build | `Makefile` | builds the Go binaries |
| runner | `run.sh` | both services, one after the other, plus the table |

The kernel is not in this directory: it is `lib/sha256.ta` in the standard
library, and the C behind it (`lib/sha256.c`) is a lazy dylib the VM dlopens
on the first `sha256.*` reference.

`go-server`, `bench`, `fairness` and `latency` are build output and are not
in git.