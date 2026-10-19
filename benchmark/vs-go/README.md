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
| `server.ta` | TA HTTP service (lib/http.ta: listen_routes + serve_loop, one handler actor per connection, no keep-alive) |
| `server.go` | Go HTTP service (net/http), same document on every path |
| `serve_bench.go` | the shared load generator / client |
| `run.sh` | builds all three, starts each server in turn, load-tests it, prints the comparison |

Both servers answer every path with `{"message":"hello","ok":true}` — the
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

## serve_bench.go — also usable against lib/serve.ta

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
| `-k` | off | keep-alive (off by default: serve.ta closes each conn) |
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