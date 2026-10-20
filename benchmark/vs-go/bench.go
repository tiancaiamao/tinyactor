// The benchmark's client: draws each request's size 80/20 from a per-worker
// seeded PRNG, so both servers are handed the same sizes in the same order.
//   go run bench.go -url http://127.0.0.1:8099
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
	baseURL  = flag.String("url", "http://127.0.0.1:8099", "base URL")
	conc     = flag.Int("c", 16, "request concurrency")
	dur      = flag.Duration("d", 12*time.Second, "duration of the measured window")
	heavyPct = flag.Int("heavypct", 20, "percent of requests that are heavy")
	lightN   = flag.Int("lightn", 20000, "passes in a light request")
	heavyN   = flag.Int("heavyn", 2000000, "passes in a heavy request")
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

// draw is a per-worker LCG: worker i issues the same size sequence for both
// servers, so the two sides are handed identical work.
type draw struct{ s uint64 }

func (d *draw) next() uint64 {
	d.s = d.s*6364136223846793005 + 1442695040888963407
	return d.s >> 33
}

func main() {
	flag.Parse()
	path := func(n int) string { return fmt.Sprintf("%s/work/%d", *baseURL, n) }

	tr := &http.Transport{MaxIdleConnsPerHost: 2 * *conc}
	client := &http.Client{Transport: tr, Timeout: 5 * time.Minute}

	var heavyInFlight atomic.Int64
	light, heavy := &bucket{}, &bucket{}

	get := func(n int) time.Duration {
		t0 := time.Now()
		resp, err := client.Get(path(n))
		if err != nil {
			return 0
		}
		io.Copy(io.Discard, resp.Body)
		resp.Body.Close()
		return time.Since(t0)
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
					heavyInFlight.Add(1)
					if dt := get(*heavyN); dt == 0 {
						heavy.err()
					} else {
						heavy.add(dt)
					}
					heavyInFlight.Add(-1)
					continue
				}
				if dt := get(*lightN); dt == 0 {
					light.err()
				} else {
					light.add(dt)
				}
			}
		}(i)
	}

	start := time.Now()
	time.Sleep(*dur)
	close(done)
	wg.Wait()
	secs := time.Since(start).Seconds()

	fmt.Printf("c=%d duration=%s  heavy=%d%% (%d passes), light=%d%% (%d passes)\n\n",
		*conc, *dur, *heavyPct, *heavyN, 100-*heavyPct, *lightN)
	light.report("light")
	heavy.report("heavy")
	fmt.Printf("%-14s light %.0f/s  heavy %.0f/s  total %.0f/s\n", "throughput",
		float64(light.count())/secs, float64(heavy.count())/secs,
		float64(light.count()+heavy.count())/secs)
}