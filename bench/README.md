# bench — HTTP load generator for lib/serve.ta

`serve_bench.go` is a small Go load generator (stdlib only, no deps)
used to exercise the demo static file server `lib/serve.ta` and to
produce CPU profiles of the VM.

## Usage

```sh
go run bench/serve_bench.go -url http://127.0.0.1:8099/ -c 50 -n 3000
```

| flag | default | meaning |
|------|---------|---------|
| `-url` | `http://127.0.0.1:8099/` | target URL |
| `-c` | `20` | concurrent workers |
| `-n` | `2000` | total requests (ignored if `-d` is set) |
| `-d` | `0` | run for a duration instead (e.g. `-d 10s`) |
| `-k` | off | keep-alive (off by default: serve.ta closes each conn) |

Output: throughput (req/s), bandwidth, avg / p50 / p90 / p99 / max
latency, and the number of failed requests.

## Profiling the server

1. Compile `serve.ta` (TA source) to bytecode:

   ```sh
   ./tavm lib/bootstrap.tabc lib/serve.ta /tmp/serve.tabc
   ```

2. Run with sampling enabled. `--profile=serve` writes
   `serve.json` + `serve.folded` in the current directory on exit:

   ```sh
   ./tavm --profile=serve /tmp/serve.tabc 8099 ~/project/tiancaiamao.github.io
   ```

3. In another terminal, load it:

   ```sh
   go run bench/serve_bench.go -url http://127.0.0.1:8099/ -c 50 -n 3000
   ```

4. Ctrl-C the server — the SIGINT handler stops the VM gracefully and
   flushes the profile. Then:

   ```sh
   ls serve.json serve.folded
   open serve.json     # or drag into https://www.speedscope.app/
   sort -rn serve.folded | head -20   # top stacks as text
   ```

Notes:

- `--profile` (no value) writes `profile.json` / `profile.folded`.
- The profiler samples stacks on worker threads and attributes time to
  TA functions (real names from the `.tabc` v2 function table).
- `NWORKERS=4 ./tavm ...` overrides the worker-thread count.