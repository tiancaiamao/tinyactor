# benchmark/vs-go — TinyActor vs Go, end to end

One load-testing client (`serve_bench.go`), two servers serving the
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
| `serve_bench.go` | the shared load generator / client (homogeneous light load) |
| `run.sh` | builds all three, starts each server in turn, load-tests it, prints the comparison |
| `fair_bench.go` | mixed-load fairness client: light stream + one heavy request, bucketed latency |
| `fair.sh` | the fairness comparison (`make benchmark-fair`) — see "Mixed-load fairness" below |

Both servers answer `/` and `/light` with `{"message":"hello","ok":true}` — the
benchmark measures the runtime's HTTP path, not application logic. Runs are
**sequential** (TA, then Go) so the two never fight for CPU; a 1s warmup
precedes each measured window.

## The one non-obvious knob: `-rst`

The client opens a fresh connection per request (TA's http layer v1 has no
keep-alive, so this keeps both sides apples-to-apples). On loopback that
exhausts the client's ephemeral port range (49152–65535) within seconds:
each actively-closed socket sits in `TIME_WAIT` for ~30s, which caps
throughput at ~16384/30 ≈ **500 req/s for any server, regardless of
concurrency** — a pure OS artefact that hides the real difference.

`-rst` (on by default) closes each socket with `SO_LINGER=0` (RST instead of
FIN), freeing the source port immediately, so the cap disappears. Run with
`-rst=false` to reproduce the raw `TIME_WAIT` ceiling.

## Reading the numbers

- **Machine- and load-sensitive.** Client and server share this box, so both
  compete for the same cores. Compare within one run, not across machines.
- Env knobs: `CONC` (default 32), `DUR` (default 5s), `NWORKERS` (TA worker
  threads, default ncpu), `GO`.
- The shape to look at is the whole **latency distribution** (p50 → p99 →
  max), not just throughput. The summary prints `tail p99/p50` as a compact
  tail-tightness number.
- **`max` is misleading here.** The TA server's heap grows under sustained
  load (RSS climbs to ~1.4 GB at ~15 k req/s, reclaimed once load stops — not
  a leak, but real memory churn). On a memory-tight box that drives paging,
  so TA shows occasional multi-second `max` spikes that the Go side does not,
  and its throughput drifts down on longer runs. Keep `DUR` short (the 5s
  default) and judge the tail by p99, not `max`.

## Sample output (Apple M3, 8 cores)

```
tinyactor   20836 req/s   p50 937.709µs   p90 3.061167ms   p99 4.981833ms   max 3.512395042s
go          41821 req/s   p50 620.333µs   p90 991.084µs    p99 1.771125ms   max 105.305375ms
```

Go is ~1.8–2.0× the throughput on this trivial service and its tail is
tighter (TA's p99 is ~2.6× Go's). Absolute values move with concurrency,
duration and machine; the ratio is the signal.

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
flight for its whole lifetime, and reports the two distributions:

```
concurrency=16  duration=6s  warmup=2s  heavy=400M rounds chunk=2000 (both sides)

--- tinyactor (server.ta, NWORKERS=1) ---
baseline (no heavy)    n=230313  p50=0.352  p90=0.370  p99=0.488  max=4.024  errs=0
during heavy           n=3216    p50=4.358  p90=4.390  p99=4.428  max=4.555  errs=0
heavy request          873ms

--- go (server.go, GOMAXPROCS=1) ---
baseline (no heavy)    n=880009  p50=0.090  p90=0.151  p99=0.273  max=1.163  errs=0
during heavy           n=488     p50=27.481 p90=30.054 p99=30.290 max=30.359 errs=0
heavy request          799ms

--- summary: light latency inflation (during-heavy / baseline) ---
side            p50x       p90x       p99x       maxx
tinyactor      12.38      11.86       9.07       1.13
go            305.34     199.03     110.95      26.10
```

Reading it:

* **Neither runtime is magic.** Both inflate light latency while the heavy
  request runs — one worker is one worker; Go's baseline light path is also
  ~4× faster (0.09 ms vs 0.35 ms), which is the throughput story.
* **The inflated latency is the signal.** TA holds light p50 at **4.4 ms** with
  p99 = 4.4 ms — a tight, *deterministic* bound set by the yield granularity —
  while Go spikes to **27.5 ms** with a 30 ms tail. In absolute terms TA's light
  requests are delayed ~6× less, and TA serves **3216** light requests during
  the heavy window vs Go's **488** (~6× more work delivered).
* **Why.** The TA burn actor crosses the VM reduction budget every ~500 chunks
  (`chunk=2000` ≈ 2 ms of C) and yields; the Go heavy goroutine is only
  preempted by the runtime's ~10 ms async signal, after which the returning
  light goroutines must wait for the P. TA's fairness granularity is a property
  of the *loop shape* (cost: a few extra interpreter reductions per slice), not
  a ~10 ms timer.

A second run reproduced the p50 story (tinyactor 4.65 ms vs go 24.9 ms) and the
per-window light counts (3187 vs 538). Tail percentiles move more between runs
(machine noise) — read **p50 and the served-count**, not p99.

### Chunk size is the knob — and it is the honest caveat

The bound above is bought with chunking, and the same mechanism shows why:

* `chunk=2000` → heavy 873 ms, light p50 during heavy **4.36 ms**.
* `chunk=800000000` (one giant C call — an *unpreemptible*, cgo-shaped call) →
  a probe of the same pattern (`compute.burn(800000000, 800000000)` with a
  20 ms sleep loop in another actor, NWORKERS=1) shows the light actor starved
  for the whole 1.6 s burn: first wake at `t+1594 ms`, versus a steady
  `t+23 46 69 93 …` when chunked. That is the control: the yield boundary, not
  the C code, is what provides the fairness.

Smaller chunks tighten the bound and pay more interpreter overhead per round.
Both sides keep the same loop shape, so the difference measured is policy.



`serve_bench.go` doubles as a general load generator / CPU-profiler driver
for the demo static-file server `lib/serve.ta`.

```sh
go run benchmark/vs-go/serve_bench.go -url http://127.0.0.1:8099/ -c 50 -n 3000
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
   go run benchmark/vs-go/serve_bench.go -url http://127.0.0.1:8099/ -c 50 -n 3000
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