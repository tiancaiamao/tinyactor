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
same 64KB blocks, and both answer with the digest — so the runner can compare
the two services' responses byte for byte. Same work, or the comparison is
worthless.

The client draws the sizes 80/20 from a per-worker seeded PRNG, so **both
servers are handed the same sizes in the same order**.

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
`make benchmark-vs-go-sched` runs it with the latency-sensitive probes on,
`make benchmark-vs-go-fairness` runs the fairness case.

`run.sh` takes two knobs:

| knob | default | what it does |
|---|---|---|
| `WORKERS` | 1 | workers per side: `NWORKERS` on the tinyactor side, `GOMAXPROCS` on the Go side |
| `DELAYC` | 0 | latency-sensitive probes against `POST /delay1ms`, each on its own connection. 4 is the sched case |

```sh
cd benchmark/vs-go
DELAYC=4 ./run.sh              # the sched case, one worker each
WORKERS=6 DELAYC=4 ./run.sh    # the sched case, six workers each
```

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
digest load itself is identical on both sides — same sizes, same order, and
the runner checks that the two services answered with the same bytes
(`same work: yes`).

### One worker each

M3, c=16, 12s, 80/20, 4 probes. Latency in ms; ratio < 1 = tinyactor faster.

| | tinyactor | go | ta/go |
|---|---|---|---|
| **delay p50** | **7.96** | **28.14** | **0.28×** |
| **delay p99** | **16.04** | **62.27** | **0.26×** |
| light p50 | 15.21 | 17.02 | 0.89× |
| light p99 | 31.32 | 44.66 | 0.70× |
| heavy p50 | 36.15 | 22.49 | 1.61× |
| heavy p99 | 58.39 | 50.44 | 1.16× |
| delay req/s | 491 | 140 | 3.51× |
| light req/s | 646 | 686 | 0.94× |
| heavy req/s | 165 | 176 | 0.94× |
| total req/s | 1302 | 1003 | 1.30× |
| cores used | 1.01 | 1.00 | |

The latency-sensitive endpoint answers **3.6× faster at p50 and 3.9× faster at
p99**, while the digest load runs at the same rate on both sides (0.94×) — and
tinyactor serves 30% more requests in total, because it gets through the
probes three and a half times as fast.

### Six workers each

Same load, six workers per side — the shape a real service runs in:

| | tinyactor | go | ta/go |
|---|---|---|---|
| **delay p50** | **1.96** | **8.76** | **0.22×** |
| **delay p99** | **4.37** | **25.22** | **0.17×** |
| light p50 | 2.30 | 0.57 | 4.03× |
| light p99 | 5.35 | 14.97 | 0.36× |
| heavy p50 | 11.78 | 7.99 | 1.48× |
| heavy p99 | 19.69 | 22.12 | 0.89× |
| delay req/s | 1973 | 396 | 4.98× |
| light req/s | 2931 | 3437 | 0.85× |
| heavy req/s | 731 | 855 | 0.85× |
| total req/s | 5635 | 4688 | 1.20× |
| cores used | 5.76 | 5.86 | |

Eight workers: delay p50 1.48 vs 8.30 (0.18×), delay p99 4.29 vs 25.71
(0.17×), total 6836 vs 5092 (1.34×), cores used 6.89 vs 6.55.

Go's delay latency is one timeslice (~10ms) once there are enough Ps to run
the digest load in parallel — it does not get better at eight — while
tinyactor's does not get worse.

### Why Go is slow here, and why tinyactor is not

The delay request does one millisecond of sleeping and almost no work. What
the client measures on top of that millisecond is how long the runtime takes
to run it again.

*On the Go side:* a goroutine that wakes from a timer needs a P to run on. The
P is busy with the digest requests, and Go's scheduler has no notion of a
latency-sensitive task — the sched repo calls the Go runtime "barely FIFO" —
so the woken goroutine waits behind the running one until the scheduler hands
the P over. The 1ms sleep becomes 28ms at p50 and 62ms at p99.

*On the tinyactor side:* the VM charges a reduction per control transfer and
hands the worker back when the slice budget (1000 reductions) is spent
(`src/vm.c:395`, `src/scheduler.c:587`). `time.sleep` parks the actor on the
poller's deadline, and the poller enqueues it again the moment that deadline
passes (`src/scheduler.c:740`); the worker that comes back to the run queue
picks it up. So the wait is one slice — tens of microseconds — not one
timeslice.

Same machine, same load, same client, same protocol: the difference is the
scheduler's idea of what deserves to run next.

## The digest service

The same comparison without the probes — the service on its own, where the
two runtimes are at parity.

Six workers each (c=16, 12s, 80/20). Latency in ms; ratio < 1 = tinyactor
faster.

| | tinyactor | go | ta/go |
|---|---|---|---|
| light p50 | 2.07 | 0.55 | 3.76× |
| light p99 | 4.96 | 14.73 | 0.34× |
| heavy p50 | 10.97 | 7.95 | 1.38× |
| heavy p99 | 19.73 | 22.78 | 0.87× |
| light req/s | 3193 | 3447 | 0.93× |
| heavy req/s | 796 | 857 | 0.93× |
| total req/s | 3989 | 4304 | 0.93× |
| cores used | 5.92 | 5.86 |

Same work, same load, 7% apart in throughput — and the shape of the latency
distribution is different on the two sides: tinyactor's small requests have a
worse median (3.8×) and a much better tail (2.9× better at p99). That is the
coarse scheduler showing: the slices are big enough that a request waits for
the current one, but the wait is bounded, so the tail stays tight. Go
preempts at every function call, so the median is low and the tail is long.

### What the request actually costs

One client at a time, no queueing — the per-request cost of each runtime
(`./bench -c 1 -heavypct 0`):

| | tinyactor | go |
|---|---|---|
| light request (64KB digest) | 0.120 ms | 0.050 ms |
| light req/s (one client, pure light) | 8205/s | 19241/s |

Two things to read out of it:

* **The kernel costs the same on both sides** — a 1KB request costs 0.097 ms on
  tinyactor and 0.028 ms on go; the same request with 64KB costs 0.120 ms and
  0.050 ms — the extra 0.023 ms and 0.022 ms is the SHA-256 of the 63KB more,
  i.e. ~3 GB/s on both sides.
* **The request path is TA code** — HTTP head parsing, JSON in and out, the
  actor dispatch. 0.097 ms against Go's 0.028 ms is the whole 2.4× on the
  light path. It is the runtime being compared, not the kernel.

### Where the mixed numbers come from

With one worker, the heavy requests are ~90% of the CPU (16MB of SHA-256
each), so the run is a study of **how each runtime hands that worker back**:

* The tinyactor VM charges a reduction per control transfer and returns the
  worker when the slice budget (1000) is spent. A 16MB digest is 256 chunks
  of 64KB, so a heavy request hands the worker back about 7 times (the VM
  profiler counts the budget exhaustions: 7 per request with 64KB chunks, 28
  with 16KB chunks).
* Go's goroutine is preemptible at every function call, so the 256 chunks of a
  heavy request interleave with the other connections' requests 256 times.

The two distributions put the wait in different places. On the tinyactor side
the light requests are served sooner (p50 15.2 ms, p99 31.3 ms, against
17.0/44.7), and the heavy ones later (p50 36.2 ms, against 22.5 ms). Same
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

## The fairness case

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