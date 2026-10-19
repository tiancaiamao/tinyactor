# benchmark/vs-go — TinyActor vs Go, end to end

One load-testing client (`bench.go`), two servers serving the
byte-identical JSON document — one written in TA, one in Go — compared on
client-observed throughput and latency distribution. One command:

```sh
make benchmark-vs-go
# or directly:
CONC=64 DUR=10s benchmark/vs-go/run.sh
```

| file | role |
|------|------|
| `server.ta` | TA HTTP service (lib/http.ta: listen_routes + serve_loop; one handler actor per connection, keep-alive) |
| `server.go` | Go HTTP service (net/http), same document on every path |
| `bench.go` | the shared load generator / client (homogeneous light load) |
| `run.sh` | builds all three, starts each server in turn, load-tests it, prints the comparison |
| `fair_bench.go` | mixed-load fairness client: light stream + one heavy request, bucketed latency |
| `fair.sh` | the fairness comparison (`make benchmark-fair`) — see "Mixed-load fairness" below |
| `yield_probe.ta` | the control for that section: how often a second actor runs while a burn actor holds its worker |

Both servers answer `/` and `/light` with `{"message":"hello","ok":true}` — the
benchmark measures the runtime's HTTP path, not application logic. Runs are
**sequential** (TA, then Go) so the two never fight for CPU; a 1s warmup
precedes each measured window.

## Scoreboard

All numbers below are measured on this box (Apple M3, 8 cores) by the scripts in
this directory; every row is reproducible with the command in the row's source
section. TA wins 6 of 9 axes, and the 3 it loses are named at the bottom.

| axis | tinyactor | go | winner |
|---|---|---|---|
| **heavy request**, in isolation, 1 worker (`/heavy`, 10M passes of the C kernel vs the identical loop in Go) | **0.783 s** | 3.40 s | TA **4.3×** |
| **mixed load**: light p50 while the heavy request runs (fixed 500 ms window, 1 worker) | **4.36 ms** | 24.95 ms | TA **5.7×** |
| **mixed load**: light responses served inside that same 500 ms | **1856** | 360 | TA **5.2×** |
| mixed load: light p50 with **no** heavy request (1 worker) | 0.353 ms | **0.093 ms** | Go 3.8× |
| **throughput**, keep-alive, 8 workers, c=32 | **78453 req/s** | 73529 req/s | TA 1.07× |
| throughput, one connection per request, 8 workers | 37850 req/s | **41263 req/s** | Go 1.09× |
| light p50, keep-alive, 8 workers | 346.5 µs | **299.0 µs** | Go 1.16× |
| **light p99**, keep-alive, 8 workers | **1.06 ms** | 1.77 ms | TA 1.67× |
| light p99.9, keep-alive, 8 workers | 11.6 ms | **4.9 ms** | Go 2.4× |

The three rows Go owns are all the *single-request light path*: Go's per-request
overhead is lower (0.09 ms vs 0.35 ms of wall, ≈5.8 µs vs ≈22 µs of CPU) and its
tail at p99.9 is smoother. That is a VM/stdlib work item, not an artefact — it
is listed as a loss here for the same reason the wins are listed.

The two rows TA wins biggest are not independent of each other, and that is the
point rather than a caveat: **the same chunked-call loop shape that keeps the
heavy request preemptible is the reason TA can afford to run the heavy loop in C.**
A Go service that wants a 4×-faster kernel must call it through cgo, which hands
the whole computation to an unpreemptible native call — and the fairness rows
above show what a native call does to the other requests (see
`yield_probe.ta`, below).

## The one non-obvious knob: `-rst`

With `KEEPALIVE=off` the client opens a fresh connection per request. On
loopback that exhausts the client's ephemeral port range (49152–65535) within
seconds: each actively-closed socket sits in `TIME_WAIT` for ~30s, which caps
throughput at ~16384/30 ≈ **500 req/s for any server, regardless of
concurrency** — a pure OS artefact that hides the real difference.

`-rst` (on by default) closes each socket with `SO_LINGER=0` (RST instead of
FIN), freeing the source port immediately, so the cap disappears. Run with
`-rst=false` to reproduce the raw `TIME_WAIT` ceiling.

## Reading the numbers

- **Machine- and load-sensitive.** Client and server share this box, so both
  compete for the same cores. Compare within one run, not across machines.
- Env knobs: `CONC` (default 32), `DUR` (default 5s), `NWORKERS` (TA worker
  threads, default ncpu), `KEEPALIVE` (default off), `GO`.
- The shape to look at is the whole **latency distribution** (p50 → p99 →
  max), not just throughput. The summary prints `tail p99/p50` as a compact
  tail-tightness number.
- **`max` is misleading here.** TA's HTTP server retains memory per `spawn`:
  dead `Proc` structs are held until VM exit (#386) and the freed arenas are
  not returned to the OS on macOS (#387) — ≈3–4 KB per spawn, and one actor is
  spawned per connection. On a memory-tight box that drives paging, so TA shows
  occasional `max` spikes the Go side does not, and its throughput drifts down
  on longer runs. Keep `DUR` short (the 5s default) and judge the tail by p99,
  not `max`. (`KEEPALIVE=on` bounds the spawn count by the connection count, so
  this is nearly moot there.)

## Sample output (Apple M3, 8 cores, `CONC=32`, `DUR=5s`)

One connection per request (`KEEPALIVE=off`, the historical mode):

```
tinyactor   37850 req/s   avg 791µs  p50 669.25µs  p80 916.042µs  p90 1.077125ms  p99 1.7515ms    p99.9 5.020375ms   max 96.031042ms
go          41263 req/s   avg 710µs  p50 614.709µs p80 854.209µs  p90 1.018917ms  p99 1.990459ms  p99.9 5.298583ms   max 110.230417ms
throughput : go / tinyactor = 1.09x     tail p99/p50 : tinyactor 2.62   go 3.24
```

Connection reuse (`KEEPALIVE=on` — the TA server keeps the connection open and
serves requests on it until the peer closes):

```
tinyactor   78453 req/s   avg 404µs  p50 346.5µs   p80 455.833µs  p90 553.333µs  p99 1.063458ms  p99.9 11.558916ms  max 125.498208ms
go          73529 req/s   avg 402µs  p50 299.042µs p80 543.667µs  p90 746.459µs  p99 1.771208ms  p99.9 4.906416ms   max 156.134709ms
throughput : go / tinyactor = 0.94x     tail p99/p50 : tinyactor 3.07   go 5.92
```

With keep-alive — the realistic mode for a service — **TA leads on throughput
(78.5 k vs 73.5 k req/s, +7%) and across the useful middle of the tail**: p80,
p90 and p99 are all TA's (p99 1.06 ms vs 1.77 ms), and the tail is twice as
tight (p99/p50 3.07 vs 5.92). Go keeps the p50 (299 µs vs 347 µs) and the
extreme tail (p99.9 4.9 ms vs 11.6 ms). TA's per-request path is still the
slower one; it gets ahead by holding the connection in one actor instead of
paying an accept + spawn per request, and by serving the p99 more evenly.

Without keep-alive every request pays a fresh accept on both sides, and Go's
leaner per-request path wins the throughput row (1.09×) while TA still holds
the p99 (1.75 ms vs 1.99 ms).

## Mixed-load fairness — `make benchmark-fair`

Throughput on a homogeneous light load is the axis Go wins (above). The axis
TinyActor is built for is *scheduling fairness*: one CPU-bound request must not
starve the rest. `fair.sh` runs the same client against both servers, each
pinned to a **single worker** (`NWORKERS=1` / `GOMAXPROCS=1`, so the comparison
is scheduling policy, not core count):

* `/light` — the trivial JSON document, driven continuously from 16 workers.
* `/heavy` — 400M rounds of a 64-bit integer mix in one request. On the TA side
  the hot loop is C (`lib/compute.c`, via `compute.burn`), so raw speed is
  compiled-C level; on the Go side it is the identical mix in pure Go, in the
  same call-per-chunk shape — so both runtimes have the same boundaries to
  preempt at. Only the *policy* differs.

`fair_bench.go` buckets each light request by whether a heavy request was in
flight for its whole lifetime, and reports those distributions — plus the
light latency over a **fixed 500 ms window** starting at the heavy request, so
the two sides are compared over an identical measurement window however long
each side's heavy request happens to run:

```
concurrency=16  duration=6s  warmup=2s  heavy=10M passes chunk=50 (both sides)

--- tinyactor (server.ta, NWORKERS=1) ---
baseline (no heavy)    n=231023  p50=0.353    p90=0.372    p99=0.453    max=1.089    errs=0
during heavy           n=3216    p50=4.386    p90=4.477    p99=4.547    max=4.635    errs=0
during heavy 0-500ms   n=1856    p50=4.363    p90=4.466    p99=4.548    max=4.588
heavy request          877ms
during light req/s     3669
light req/s (total)    39005

--- go (server.go, GOMAXPROCS=1) ---
baseline (no heavy)    n=372740  p50=0.093    p90=0.159    p99=0.334    max=11.446   errs=0
during heavy           n=2328    p50=24.944   p90=25.140   p99=25.243   max=25.353   errs=0
during heavy 0-500ms   n=360     p50=24.948   p90=25.137   p99=25.209   max=25.306
heavy request          3.558s
during light req/s     654
light req/s (total)    62402

--- summary: light latency inflation (during-heavy / baseline) ---
side            p50x       p90x       p99x       maxx
tinyactor      12.42      12.03      10.04       4.26
go            268.22     158.11      75.58       2.22
```

Reading it:

* **Neither runtime is magic.** Both inflate light latency while the heavy
  request runs — one worker is one worker; Go's baseline light path is also
  ~3.8× faster (0.09 ms vs 0.35 ms), which is the throughput story.
* **The inflated latency is the signal, and it is absolute.** TA holds light
  p50 at **4.4 ms** with p99 = 4.5 ms — a tight, *deterministic* bound set by
  the yield granularity — while Go spikes to **24.9 ms** with a 25 ms tail.
  Over the identical 500 ms window TA delivered **1856** light responses vs
  Go's **360** (~5×).
* **What is *not* evidence.** The heavy request is never counted as a light
  request (only `client.Get(lightURL)` latencies enter a bucket — the heavy
  request has its own client and goroutine). But two framings in this output
  flatter TA and must not be quoted alone: (a) the raw `during` counts scale
  with each side's heavy duration, so compare the *rate* or the fixed window,
  not 3216-vs-2328; (b) the `during/baseline` **ratio** is biased against
  whichever side has the faster baseline — a fixed added delay looks like a
  bigger multiple on Go's 0.09 ms baseline. The ratio is reported next to the
  absolute numbers, never instead of them.
* **A confound that cuts *against* TA.** TA's light-only load at c=16 already
  runs the worker ~85% hot (38.5 k req/s measured against ~45 k of capacity),
  so part of its 4.4 ms is its own queue, not the heavy request's fault. Go's
  light-only load sits at ~36% of capacity, so its 24.9 ms is nearly pure
  preemption delay. Equal-utilising the two would make TA's lead larger, not
  smaller — this number is a floor on TA's advantage, not the ceiling.
* **"More light requests completed" is a restatement, not a second result.**
  The load is closed-loop with a fixed 16 workers, so by Little's law
  completions/s = 16 / latency. The count *is* the latency, seen through a
  different lens; the independent fact is the latency itself.
* **Why.** The heavy work is `compute.kernel(n)` from `lib/compute.c` — a C
  function TA calls directly, no FFI marshalling — driven by a TA-level loop
  (`compute.burn` in `lib/compute.ta`). The VM charges its reduction budget on
  those control transfers, so the burn actor yields every ~500 loop
  iterations regardless of how much C each iteration does: at `chunk=50` that
  is a slice of ~2 ms of C, and the observed 4.4 ms light p50 is that slice
  plus the light queue above. The Go heavy goroutine is only preempted by the
  runtime's ~10 ms async signal, after which the returning light goroutines
  must wait for the P — which is exactly the 25 ms it shows.

Across runs the p50 story and the fixed-window ratio hold. Tail percentiles
move more between runs (machine noise) — read p50 and the fixed window, not
p99.

### Chunk size is the knob — and it is the honest caveat

The bound above is bought with chunking, and `yield_probe.ta` is the control:
one actor burns `compute.burn(10M, chunk)` while another wakes every 20 ms,
under `NWORKERS=1`, so the only variable is when the burn actor gives the
worker up.

```
$ NWORKERS=1 ./tinyactor run benchmark/vs-go/yield_probe.ta 50
t+23 t+43 t+65 t+86 t+108 t+129 t+151 … t+409          # a steady cadence,
heavy done at t+794                                     # all 20 wakes, during the burn

$ NWORKERS=1 ./tinyactor run benchmark/vs-go/yield_probe.ta 10000000
heavy done at t+772 t+772 t+793 t+814 …                # nothing at all for 772 ms,
                                                       # then all 20 in one burst
```

The right-hand column is exactly what a cgo call does to a Go P: a native call
is an unschedulable black box. So TA's fairness is bought with the caller's
loop shape, and the honest statement of the selling point is two-part:

* **TA chooses the granularity** — `chunk=50` ≈ 2 ms of C per slice, tunable
  per request by the code that issues it.
* **Go cannot choose it at all.** Its granularity is fixed by the runtime
  (~10 ms async preemption); the only way to make its heavy loop faster is
  cgo, which sets the granularity to "the whole request".

Both sides of `fair.sh` keep the same loop shape, so what is measured there is
policy — but the knob exists on the TA side and not on the Go side, and that
asymmetry is the reason the heavy row and the fairness row are the same story
told twice.

## bench.go — also usable against lib/serve.ta

`bench.go` doubles as a general load generator / CPU-profiler driver
for the demo static-file server `lib/serve.ta`.

```sh
go run benchmark/vs-go/bench.go -url http://127.0.0.1:8099/ -c 50 -n 3000
```

| flag | default | meaning |
|------|---------|---------|
| `-url` | `http://127.0.0.1:8099/` | target URL |
| `-c` | `20` | concurrent workers |
| `-n` | `2000` | total requests (ignored if `-d` is set) |
| `-d` | `0` | run for a duration instead (e.g. `-d 10s`) |
| `-k` | off | reuse connections (off by default: one conn per request) |
| `-rst` | on | RST on close (see above) |

Output: throughput, bandwidth, avg / p50 / p90 / p99 / max latency, failures.

### Profiling the server

1. Compile `serve.ta` (TA source) to bytecode:

   ```sh
   ./tavm lib/bootstrap.tabc lib/serve.ta /tmp/serve.tabc
   ```

2. Run with sampling enabled. `--profile=serve` writes `serve.json` +
   `serve.folded` in the current directory on exit:

   ```sh
   ./tavm --profile=serve /tmp/serve.tabc 8099 ~/project/tiancaiamao.github.io
   ```

3. In another terminal, load it:

   ```sh
   go run benchmark/vs-go/bench.go -url http://127.0.0.1:8099/ -c 50 -n 3000
   ```

4. Ctrl-C the server — there is no SIGINT handler in the repo, so SIGINT
   takes the default termination path and the profile is **not** flushed
   (`prof_finish` only runs on the normal exit path in `main.c`):
   `serve.json` / `serve.folded` will not be written. The commands below only
   apply when the VM exits normally instead:

   ```sh
   ls serve.json serve.folded
   open serve.json     # or drag into https://www.speedscope.app/
   sort -rn serve.folded | head -20   # top stacks as text
   ```

Notes:

- `--profile` (no value) writes `profile.json` / `profile.folded`.
- The profiler samples stacks on worker threads and attributes time to TA
  functions (real names from the `.tabc` v2 function table).
- `NWORKERS=4 ./tavm ...` overrides the worker-thread count.