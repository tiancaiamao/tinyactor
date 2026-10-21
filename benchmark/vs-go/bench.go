// The benchmark's client: it drives the mix, the server only offers the
// endpoints.
//   ./bench -url http://127.0.0.1:8099
//   ./bench -url http://127.0.0.1:8099 -fairness
//
// The service has four endpoints and no parameters — the workload is
// part of the protocol, so both sides do exactly the same work:
//
//	POST /light     -> {"bytes":65536,"sha256":"<hex>"}    (64KB digest)
//	POST /heavy     -> {"bytes":16777216,"sha256":"<hex>"}  (16MB digest)
//	POST /delay1ms  -> {"delay_ms":1}
//	POST /fairness  -> {"counts":[c0,...,c31]}
//
// The client decides what runs: -heavypct is the share of the request mix
// that goes to /heavy, -delayc is the number of latency-sensitive probes,
// -fairness is the scheduler's fairness case (one POST /fairness, the
// counts come back and the client computes the deviation).
// One request in VERIFY_EVERY is checked against a digest the client
// computed itself — the client is the only side not under test, so it is
// the one that can afford the extra hashing.
//
// -delayc runs that many latency-sensitive probes (POST /delay1ms) against
// the same server, each on its own connection, in their own pool. The
// probes are separate from the request mix on purpose: a probe that is slow
// must not take capacity away from the load it measures, or the two sides
// would not be doing the same work. The server sleeps a millisecond, so
// everything above the millisecond is the scheduler's wake-up latency.
package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"sort"
	"sync"
	"sync/atomic"
	"time"
)

const seed = "tinyactor-benchmark-payload-seed-v1-0123456789abcdefghijklmnopxy"

// The payload sizes of the protocol — the same constants the servers use.
const (
	lightBytes = 65536
	heavyBytes = 16777216
)

// VERIFY_EVERY: how many responses are checked against a locally computed
// digest. The check hashes the whole document, so it is sampled.
const VERIFY_EVERY = 64

var (
	baseURL  = flag.String("url", "http://127.0.0.1:8099", "base URL")
	conc     = flag.Int("c", 16, "request concurrency")
	dur      = flag.Duration("d", 12*time.Second, "duration of the measured window")
	heavyPct = flag.Int("heavypct", 20, "percent of the request mix that goes to /heavy")
	delayC   = flag.Int("delayc", 0, "concurrent latency-sensitive probes (POST /delay1ms)")
	fairness = flag.Bool("fairness", false, "the fairness case: one POST /fairness, print the deviation")
)

// delayBody: the answer /delay1ms gives on both sides.
const delayBody = `{"delay_ms":1}`

type bucket struct {
	mu   sync.Mutex
	ms   []float64
	errs int64
}

func (b *bucket) add(d time.Duration) {
	b.mu.Lock()
	b.ms = append(b.ms, float64(d.Microseconds())/1000.0)
	b.mu.Unlock()
}

func (b *bucket) err() { atomic.AddInt64(&b.errs, 1) }

func (b *bucket) report(name string) {
	b.mu.Lock()
	ms := append([]float64(nil), b.ms...)
	errs := atomic.LoadInt64(&b.errs)
	b.mu.Unlock()
	sort.Float64s(ms)
	p := func(q float64) float64 {
		if len(ms) == 0 {
			return 0
		}
		i := int(q / 100.0 * float64(len(ms)))
		if i >= len(ms) {
			i = len(ms) - 1
		}
		return ms[i]
	}
	fmt.Printf("%-14s n=%-7d p50=%-9.3f p90=%-9.3f p99=%-9.3f max=%-9.3f errs=%d\n",
		name, len(ms), p(50), p(90), p(99), p(99.99), errs)
}

func (b *bucket) count() int {
	b.mu.Lock()
	defer b.mu.Unlock()
	return len(b.ms)
}

// expected: the digest of the first n bytes of the document — the seed
// repeated, truncated. The client computes the two protocol digests once
// at startup; the servers compute them per request.
func expected(n int) string {
	sum := sha256.New()
	for i := 0; i < n/len(seed); i++ {
		sum.Write([]byte(seed))
	}
	if r := n % len(seed); r > 0 {
		sum.Write([]byte(seed)[:r])
	}
	return hex.EncodeToString(sum.Sum(nil))
}

type response struct {
	Bytes  int64  `json:"bytes"`
	SHA256 string `json:"sha256"`
}

// draw is a per-worker LCG: worker i issues the same sequence of
// endpoints for both servers, so the two sides are handed identical work.
type draw struct{ s uint64 }

func (d *draw) next() uint64 {
	d.s = d.s*6364136223846793005 + 1442695040888963407
	return d.s >> 33
}

// runFairness: the scheduler's fairness case. The service runs the
// protocol — 32 CPU-bound workers spinning until a 3s deadline — and
// answers with the per-worker iteration counts; the deviation is computed
// here, so both sides are measured the same way.
func runFairness() {
	resp, err := http.Post(*baseURL+"/fairness", "application/json", nil)
	if err != nil {
		fmt.Fprintln(os.Stderr, "fairness request failed:", err)
		os.Exit(1)
	}
	raw, err := io.ReadAll(resp.Body)
	resp.Body.Close()
	if err != nil {
		fmt.Fprintln(os.Stderr, "fairness response failed:", err)
		os.Exit(1)
	}
	var fr struct {
		Counts []int64 `json:"counts"`
	}
	if err := json.Unmarshal(raw, &fr); err != nil {
		fmt.Fprintln(os.Stderr, "fairness response is not JSON:", err)
		os.Exit(1)
	}
	if len(fr.Counts) == 0 {
		fmt.Fprintln(os.Stderr, "fairness response has no counts")
		os.Exit(1)
	}
	mx, mn := fr.Counts[0], fr.Counts[0]
	for _, c := range fr.Counts {
		if c > mx {
			mx = c
		}
		if c < mn {
			mn = c
		}
	}
	dev := (mx - mn) * 100000 / mx
	fmt.Printf("max=%d min=%d dev_x100000=%d\n", mx, mn, dev)
}

func main() {
	flag.Parse()

	if *fairness {
		runFairness()
		return
	}

	lightDigest, heavyDigest := expected(lightBytes), expected(heavyBytes)

	tr := &http.Transport{MaxIdleConnsPerHost: 2 * *conc}
	client := &http.Client{Transport: tr, Timeout: 5 * time.Minute}

	var seq atomic.Int64
	var badResponse atomic.Int64
	light, heavy, delay := &bucket{}, &bucket{}, &bucket{}

	// post: one request against one of the digest endpoints. The path
	// carries the work; there is no body to send and nothing to parse.
	post := func(path string, n int, want string) (time.Duration, error) {
		t0 := time.Now()
		resp, err := client.Post(*baseURL+path, "application/json", nil)
		if err != nil {
			return 0, err
		}
		raw, err := io.ReadAll(resp.Body)
		resp.Body.Close()
		if err != nil {
			return 0, err
		}
		// One request in VERIFY_EVERY is checked against a digest the
		// client computes itself.
		if seq.Add(1)%VERIFY_EVERY == 0 {
			var r response
			if err := json.Unmarshal(raw, &r); err != nil || r.Bytes != int64(n) || r.SHA256 != want {
				badResponse.Add(1)
			}
		}
		return time.Since(t0), nil
	}

	// postDelay: the latency-sensitive endpoint. No body — the point is the
	// server's millisecond of sleep, not the work around it. The probes use
	// their own transport, so a probe never queues behind a digest request
	// on the client side.
	postDelay := func(c *http.Client) (time.Duration, error) {
		t0 := time.Now()
		resp, err := c.Post(*baseURL+"/delay1ms", "application/json", nil)
		if err != nil {
			return 0, err
		}
		raw, err := io.ReadAll(resp.Body)
		resp.Body.Close()
		if err != nil {
			return 0, err
		}
		if seq.Add(1)%VERIFY_EVERY == 0 && string(raw) != delayBody {
			badResponse.Add(1)
		}
		return time.Since(t0), nil
	}

	done := make(chan struct{})
	var wg sync.WaitGroup
	for i := 0; i < *conc; i++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			r := &draw{s: uint64(id)*0x9E3779B97F4A7C15 + 1}
			for {
				select {
				case <-done:
					return
				default:
				}
				if r.next()%100 < uint64(*heavyPct) {
					dt, err := post("/heavy", heavyBytes, heavyDigest)
					if err != nil {
						heavy.err()
					} else {
						heavy.add(dt)
					}
					continue
				}
				dt, err := post("/light", lightBytes, lightDigest)
				if err != nil {
					light.err()
				} else {
					light.add(dt)
				}
			}
		}(i)
	}

	// The latency-sensitive probes: their own connections, their own pool.
	// They are not part of the request mix — a probe that is slow must not
	// take capacity away from the load it measures.
	if *delayC > 0 {
		probeTransport := &http.Transport{MaxIdleConnsPerHost: 2 * *delayC}
		probeClient := &http.Client{Transport: probeTransport, Timeout: 5 * time.Minute}
		for i := 0; i < *delayC; i++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				for {
					select {
					case <-done:
						return
					default:
					}
					dt, err := postDelay(probeClient)
					if err != nil {
						delay.err()
					} else {
						delay.add(dt)
					}
				}
			}()
		}
	}

	start := time.Now()
	time.Sleep(*dur)
	close(done)
	wg.Wait()
	secs := time.Since(start).Seconds()

	fmt.Printf("c=%d duration=%s  mix: %d%% /heavy (%d bytes), %d%% /light (%d bytes), probes=%d\n\n",
		*conc, *dur, *heavyPct, heavyBytes, 100-*heavyPct, lightBytes, *delayC)
	if *delayC > 0 {
		delay.report("delay")
	}
	light.report("light")
	heavy.report("heavy")
	fmt.Printf("%-14s light %.0f/s  heavy %.0f/s  delay %.0f/s  total %.0f/s\n", "throughput",
		float64(light.count())/secs, float64(heavy.count())/secs, float64(delay.count())/secs,
		float64(light.count()+heavy.count()+delay.count())/secs)
	if bad := badResponse.Load(); bad > 0 {
		fmt.Printf("%-14s %d responses did not match the digest the client computed\n", "digest", bad)
	} else {
		fmt.Printf("%-14s every sampled response matched\n", "digest")
	}
}