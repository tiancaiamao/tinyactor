# benchmark/vs-go — TinyActor vs Go, end to end

One load-testing client, two servers doing the *same* work, compared on
client-observed throughput and latency distribution. Everything the benchmark
needs lives in this directory — including its C workload module — so nothing
here reaches into the repo's `lib/`.

```sh
make benchmark-vs-go   # homogeneous light load
make benchmark-fair    # mixed 80/20 load, one worker on each side
bash benchmark/vs-go/probe.sh 50    # the scheduling control (see below)
```

| file | role |
|------|------|
| `server.ta` | TA HTTP service (`lib/http.ta`: `listen_routes` + `serve_loop`, one handler actor per connection) |
| `server.go` | Go HTTP service (`net/http`), the same routes and the same kernel loop |
| `work.c` | the workload kernel: a 1024-entry u32 RMW loop, one call per chunk |
| `work.ta` | `work.run(seed, total, chunk)` — the chunked loop over `work.c` |
| `workenv.sh` | builds `work.c`/`work.ta` into a workdir (sourced by the shells below) |
| `bench.go` | homogeneous-light load generator |
| `fair_bench.go` | mixed-load client: draws each request's size 80/20, buckets latency |
| `run.sh` / `fair.sh` | the two comparisons |
| `yield_probe.ta` + `probe.sh` | the control for the scheduling claim |

## The workload

There is one service and one kind of request, and **how much work a request
does is a parameter of that request**: `GET /work/<n>` runs `n` passes of
`work.c` and answers with the same small JSON document as `GET /light`. `n`
must be 1..2000000 (else 400 — an out-of-range request is rejected rather
than silently answered with a smaller `n`, which would make a measurement lie
about what it ran).

The client draws those sizes 80/20 — mostly light (`n=20000`, ≈2.3 ms of C),
one in five heavy (`n=2000000`, ≈186 ms) — from a **per-worker seeded PRNG**,
so both servers are handed the same sizes in the same order by the same
worker. That is the whole scenario; nothing in it is tailored to either side.

`work.run(seed, total, chunk)` splits `total` passes into `total/chunk` native
calls, each seeded with `seed+i`, so every chunk is independent: nothing
carries between calls and the caller is free to pick any chunk. `chunk=50` is
~4 µs of C per call.

`server.go` is the same program in Go — same state array, same seed per chunk,
same recurrence, same chunk, same bounds. The two differ only in what the
compiler does with it: C vectorizes the element-parallel loop, Go emits scalar
code. (Go needs a `Sink` to stop the compiler deleting the whole loop; the TA
side cannot — the VM will not eliminate a call into a native module.)

## Scoreboard

Measured on this box (Apple M3, 8 cores) by the scripts in this directory.

| axis | tinyactor | go | winner |
|---|---|---|---|
| **kernel in isolation**, `/work/2000000`, 1 worker | **0.186 s** | 0.722 s | TA **3.9×** |
| **mixed 80/20**: total throughput, 1 worker, c=16 | **29 req/s** | 7 req/s | TA **4.1×** |
| mixed 80/20: light p50 | **62.1 ms** | 180.9 ms | TA **2.9×** |
| mixed 80/20: light p99 | **80.1 ms** | 364.5 ms | TA **4.6×** |
| mixed 80/20: heavy response p50 (under load) | **2.91 s** | 11.45 s | TA **3.9×** |
| throughput, keep-alive, 8 workers, c=32 | **73070 req/s** | 72084 req/s | TA 1.01× (parity) |
| light p99, keep-alive, 8 workers | **1.14 ms** | 1.54 ms | TA 1.35× |
| light p50, keep-alive, 8 workers | 372 µs | **315 µs** | Go 1.18× |
| light p99.9, keep-alive, 8 workers | 10.5 ms | **4.0 ms** | Go 2.6× |
| throughput, one connection per request, 8 workers | 21536 req/s | **39112 req/s** | Go 1.82× |
| **sustained load** (single server, >10 s) | collapses to 11.5 k/s, RSS 736 MB | **flat 78 k/s, RSS 17 MB** | Go — **[#389](https://github.com/tiancaiamao/tinyactor/issues/389)** |

**Read the last row first.** The TA server degrades under sustained load: RSS
grows to hundreds of MB and throughput falls to a seventh of its starting
value, and idling does not recover it. Every TA number above comes from a
window under 10 s, which is the flattering part of that curve. Until #389 is
fixed these numbers describe a cold server; they are not what a long-running
TA service delivers.

The mixed-load rows are one scenario, not four independent results: the same
faster kernel feeds all of them. What is *specifically* about scheduling is
the gap between "kernel in isolation" (3.9×) and "light p50 under load"
(2.9×) — and the control below.

## Where the mixed-load advantage comes from

Two contributions multiply, and they should not be confused:

1. **The kernel is faster** — 3.9× in isolation, same C on one side and the
   same algorithm on the other. A heavy request occupies the worker 3.9×
   less, so light requests wait behind it less. This part is C, not
   scheduling.
2. **The heavy request is preemptible at a granularity the caller picks.**
   The VM charges a reduction budget on each native call's control transfer,
   so a chunked burn gives the worker up between calls. `probe.sh` is the
   controlled experiment — same kernel, same `NWORKERS=1`, only `chunk`
   changes, with a second actor waking every 20 ms:

```
$ bash benchmark/vs-go/probe.sh 50
t+23 t+47 t+69 t+90 … t+434        # all 20 wakes on a ~20 ms cadence,
heavy done at t+964                  # throughout the burn

$ bash benchmark/vs-go/probe.sh 10000000
heavy done at t+1059                 # nothing at all for the whole burn,
t+1059 t+1080 t+1102 … t+1206        # then all 20 wakes in one burst
```

The right-hand case is exactly what a cgo call does to a Go P: one native
call is an unschedulable black box. Go's granularity is fixed by its runtime;
TA's is a parameter of the code that issues the work (`chunk`), and it is the
same knob on both the fairness client's heavy requests and the server's
handler.

Note what this does *not* claim: `chunk=50` costs something (a call per 50
passes), and a service that wants the heavy request fast rather than fair can
turn the knob the other way.

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
- Env knobs: `CONC`, `DUR`, `NWORKERS` (TA worker threads, default ncpu),
  `KEEPALIVE`, `GO`, and for the mixed load `HEAVYPCT`/`LIGHTN`/`HEAVYN`.
  Worker counts are aligned on purpose — `NWORKERS` and `GOMAXPROCS` get the
  same value, so what differs is policy and the kernel, not core count.
- The shape to look at is the whole **latency distribution** (p50 → p99 →
  max), not just throughput. `bench.go` prints `tail p99/p50` as a compact
  tail-tightness number.
- In the mixed run the light bucket split ("heavy in flight for the whole
  request") is usually empty on the no-heavy side: at 20% heavy and ~186 ms
  each, the single worker is essentially always busy with one. That is what
  the load *is*, so `fair_bench` reports it as `n/a` rather than as a ratio
  against an empty bucket.
- **Don't trust a long TA window.** See #389 above.

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
   `serve.json` / `serve.folded` will not be written. Use
   `/usr/bin/sample <pid> <seconds> -file out.txt` for a sampling profile of
   a server that is still running — that is how the profile in #389 was
   taken. The `--profile` output only applies when the VM exits normally:

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

## Why the workload module lives here, not in `lib/`

`work.c`/`work.ta` are benchmark code: no other program has a use for a
1024-entry u32 RMW loop seeded by request parameters. Putting them in `lib/`
would advertise them as stdlib. Nothing in the compiler or the VM had to
change to keep them here — the two resolution hooks that already exist are
enough:

* the compiler resolves `import "bench.vs-go:lib/work"` through the vendor
  root table, i.e. `$TA_PROJECT_ROOT/vendor/bench.vs-go/lib/work.ta`;
* the VM resolves a C module as `lib/<mod>.<ext>` relative to the server's
  CWD.

`workenv.sh` builds a workdir holding both (`lib/work.dylib` +
`vendor/bench.vs-go/lib/work.ta`), the shells run the compile half and the
servers with that workdir as their CWD, and the benchmark is self-contained.
One detail worth knowing: the compile half must be invoked *from* the
workdir, because `tinyactor` sets `TA_PROJECT_ROOT` from its own `$PWD`.