// fair_bench.go — mixed-load client for benchmark/vs-go.
//
// The scenario is a service that gets one kind of request whose cost varies:
// every request is /work/<n>, where n is the request's own size. The client
// draws those sizes 80/20 — mostly light, one in five heavy — from a per-worker
// seeded PRNG, so both servers are handed the same sizes in the same order by
// the same worker. Nothing about the load is tailored to either side.
//
// Over an identical fixed-duration window on both sides, the client reports:
//
//   - throughput, total and per class (light / heavy)
//   - light-request latency overall, and split by whether a heavy request was
//     in flight for the request's WHOLE lifetime (samples the flag before and
//     after, so straddling requests are dropped from both split buckets; the
//     overall bucket keeps them)
//   - heavy-request latency
//
// Two framings this client refuses to use, and what it reports instead:
//
//  1. Raw counts. "More light requests completed" restates the latency: the
//     load is closed loop with a fixed c workers, so by Little's law
//     completions/s = c / latency. The independent facts are the latencies and
//     the throughput rates.
//  2. The during/baseline RATIO on its own. It flatters whichever side has
//     the faster baseline, because a fixed added delay is a bigger multiple of
//     a smaller number. It is printed next to the absolute latencies, never
//     instead of them.
//
//	go run fair_bench.go -url http://127.0.0.1:8099
package main

import (
	"flag"
	"fmt"
	"io"
	"net/http"
	"sort"
	"sync"
	"sync/atomic"
	"time"
)

var (
	baseURL = flag.String("url", "http://127.0.0.1:8099", "base URL")
	conc    = flag.Int("c", 16, "request concurrency")
	dur     = flag.Duration("d", 6*time.Second, "duration of the measured window")
	heavyPct = flag.Int("heavypct", 20, "percent of requests that are heavy")
	lightN  = flag.Int("lightn", 20000, "passes in a light request")
	heavyN  = flag.Int("heavyn", 2000000, "passes in a heavy request")
)

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

func (b *bucket) addErr() { atomic.AddInt64(&b.errs, 1) }

func pct(sorted []float64, p float64) float64 {
	if len(sorted) == 0 {
		return 0
	}
	i := int(p / 100.0 * float64(len(sorted)))
	if i >= len(sorted) {
		i = len(sorted) - 1
	}
	return sorted[i]
}

func (b *bucket) report(name string) {
	b.mu.Lock()
	ms := append([]float64(nil), b.ms...)
	errs := atomic.LoadInt64(&b.errs)
	b.mu.Unlock()
	sort.Float64s(ms)
	fmt.Printf("%-20s n=%-7d p50=%-8.3f p90=%-8.3f p99=%-8.3f max=%-8.3f errs=%d\n",
		name, len(ms), pct(ms, 50), pct(ms, 90), pct(ms, 99), pct(ms, 99.99), errs)
}

// draw is a per-worker LCG: worker i issues the same size sequence for both
// servers, so the two sides are handed identical work.
type draw struct{ s uint64 }

func (d *draw) next() uint64 {
	d.s = d.s*6364136223846793005 + 1442695040888963407
	return d.s >> 33
}

func main() {
	flag.Parse()
	// +1 for the leading slash: path is /work/<n>
	heavyURL := fmt.Sprintf("%s/work/%d", *baseURL, *heavyN)
	workPath := func(n int64) string { return fmt.Sprintf("%s/work/%d", *baseURL, n) }

	tr := &http.Transport{MaxIdleConnsPerHost: 2 * *conc}
	client := &http.Client{Transport: tr, Timeout: 5 * time.Minute}

	var heavyInFlight atomic.Int64
	lightAll, baseline, during, heavyB := &bucket{}, &bucket{}, &bucket{}, &bucket{}
	var issuedLight, issuedHeavy atomic.Int64

	// get runs one request and returns its latency.
	get := func(url string) (time.Duration, error) {
		t0 := time.Now()
		resp, err := client.Get(url)
		if err == nil {
			io.Copy(io.Discard, resp.Body)
			resp.Body.Close()
		}
		return time.Since(t0), err
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
					issuedHeavy.Add(1)
					heavyInFlight.Add(1)
					dt, err := get(heavyURL)
					heavyInFlight.Add(-1)
					if err != nil {
						heavyB.addErr()
					} else {
						heavyB.add(dt)
					}
					continue
				}
				issuedLight.Add(1)
				before := heavyInFlight.Load() > 0
				dt, err := get(workPath(int64(*lightN)))
				after := heavyInFlight.Load() > 0
				if err != nil {
					lightAll.addErr()
					continue
				}
				lightAll.add(dt)
				if before && after {
					during.add(dt)
				} else if !before && !after {
					baseline.add(dt)
				}
			}
		}(i)
	}

	start := time.Now()
	time.Sleep(*dur)
	elapsed := time.Since(start).Seconds()
	close(done)
	wg.Wait()

	fmt.Printf("mixed load: c=%d duration=%s  heavy=%d%% of requests (%d passes), light=%d%% (%d passes)\n",
		*conc, *dur, *heavyPct, *heavyN, 100-*heavyPct, *lightN)
	fmt.Printf("%-20s light %d  heavy %d\n\n", "sizes issued:", issuedLight.Load(), issuedHeavy.Load())

	lightAll.report("light (all)")
	baseline.report("light, no heavy")
	during.report("light, heavy running")
	heavyB.report("heavy request")
	fmt.Printf("%-20s %s\n", "inflation", inflate(during, baseline))
	fmt.Printf("%-20s light %.0f/s  heavy %.0f/s  total %.0f/s\n", "throughput",
		float64(len(lightAll.ms))/elapsed, float64(len(heavyB.ms))/elapsed,
		float64(len(lightAll.ms)+len(heavyB.ms))/elapsed)
}

// inflate prints the during/baseline ratio, or n/a when the baseline bucket is
// empty — a ratio against an empty bucket is not zero, it is undefined.
func inflate(during, baseline *bucket) string {
	baseline.mu.Lock()
	n := len(baseline.ms)
	baseline.mu.Unlock()
	if n == 0 {
		return "n/a (no request completed without a heavy in flight)"
	}
	return fmt.Sprintf("p50x=%-6.1f p90x=%-6.1f p99x=%-6.1f", ratio(during, baseline, 50),
		ratio(during, baseline, 90), ratio(during, baseline, 99))
}

func ratio(a, b *bucket, p float64) float64 {
	b.mu.Lock()
	ams := append([]float64(nil), a.ms...)
	b.mu.Unlock()
	b.mu.Lock()
	bms := append([]float64(nil), b.ms...)
	b.mu.Unlock()
	sort.Float64s(ams)
	sort.Float64s(bms)
	base := pct(bms, p)
	if base <= 0 {
		return 0
	}
	return pct(ams, p) / base
}