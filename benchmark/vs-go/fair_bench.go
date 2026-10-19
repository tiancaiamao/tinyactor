// fair_bench.go — mixed-load fairness probe for benchmark/vs-go.
//
// One CPU-heavy request must not stall the stream of light requests. This
// client sends light requests continuously from `-c` workers and, after a
// warmup, fires exactly ONE heavy request. Each light request is bucketed by
// whether a heavy request was in flight for its whole lifetime (sampling the
// flag before and after, so straddling requests are dropped):
//
//	baseline = light latency with no heavy request in flight
//	during   = light latency while the heavy request was in flight
//
// Two honest caveats, and what this client does about them:
//
//  1. The two sides' heavy requests do not take the same wall time (same 400M
//     rounds, but an interpreter-driven loop vs a compiled one), so raw
//     `during` counts are NOT directly comparable — they scale with the heavy
//     window. Hence the during-window light request RATE is reported too.
//  2. With a closed-loop client of C workers and latency L, completions/s =
//     C/L (Little's law), so "more light requests served" is a restatement of
//     "lower light latency", not independent evidence. And the during/baseline
//     RATIO flatters whichever side has the faster baseline, so it is reported
//     alongside the absolute numbers, never instead of them.
//
// To make the two sides comparable regardless of heavy duration, the client
// also reports light latency over a FIXED window starting at the heavy
// request — the same measurement window on both sides.
//
// The fairness signal is how much the heavy request inflates light p50/p99.
// Same client for both servers, so the numbers are comparable.
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
	conc    = flag.Int("c", 16, "light-request concurrency")
	dur     = flag.Duration("d", 6*time.Second, "total light-load duration")
	warm    = flag.Duration("warm", 2*time.Second, "delay before firing the heavy request")
	lightP  = flag.String("light", "/light", "light route")
	heavyP  = flag.String("heavy", "/heavy", "heavy route")
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
	b.mu.Unlock()
	sort.Float64s(ms)
	fmt.Printf("%-22s n=%-7d p50=%-8.3f p90=%-8.3f p99=%-8.3f max=%-8.3f errs=%d\n",
		name, len(ms), pct(ms, 50), pct(ms, 90), pct(ms, 99), pct(ms, 99.99), atomic.LoadInt64(&b.errs))
}

// timed collects (offset from the heavy request's start, latency) for light
// requests, so both sides can be compared over an identical measurement window
// whatever the heavy request's own duration turns out to be.
type timed struct {
	mu  sync.Mutex
	rel []float64 // ms after the heavy request started
	lat []float64 // ms
}

func (t *timed) add(rel, lat float64) {
	t.mu.Lock()
	t.rel = append(t.rel, rel)
	t.lat = append(t.lat, lat)
	t.mu.Unlock()
}

// window returns the sorted latencies of requests that STARTED within [0, w] ms
// of the heavy request.
func (t *timed) window(w float64) []float64 {
	t.mu.Lock()
	out := make([]float64, 0, len(t.rel))
	for i, r := range t.rel {
		if r >= 0 && r <= w {
			out = append(out, t.lat[i])
		}
	}
	t.mu.Unlock()
	sort.Float64s(out)
	return out
}

func reportLats(name string, ms []float64) {
	fmt.Printf("%-22s n=%-7d p50=%-8.3f p90=%-8.3f p99=%-8.3f max=%-8.3f\n",
		name, len(ms), pct(ms, 50), pct(ms, 90), pct(ms, 99), pct(ms, 99.99))
}

func main() {
	flag.Parse()
	lightURL := *baseURL + *lightP
	heavyURL := *baseURL + *heavyP

	tr := &http.Transport{MaxIdleConnsPerHost: 2 * *conc}
	client := &http.Client{Transport: tr}
	heavyClient := &http.Client{Transport: &http.Transport{}, Timeout: 5 * time.Minute}

	var heavyOn atomic.Bool
	var heavyStart atomic.Int64 // unix nanos, 0 until the heavy request starts
	baseline, during := &bucket{}, &bucket{}
	timedS := &timed{}

	done := make(chan struct{})
	var wg sync.WaitGroup
	for i := 0; i < *conc; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for {
				select {
				case <-done:
					return
				default:
				}
				before := heavyOn.Load()
				t0 := time.Now()
				resp, err := client.Get(lightURL)
				if err == nil {
					io.Copy(io.Discard, resp.Body)
					resp.Body.Close()
				}
				dt := time.Since(t0)
				after := heavyOn.Load()
				if hs := heavyStart.Load(); hs != 0 {
					timedS.add(float64(t0.UnixNano()-hs)/1e6, float64(dt.Microseconds())/1000.0)
				}
				if err != nil {
					baseline.addErr()
					continue
				}
				if before && after {
					during.add(dt)
				} else if !before && !after {
					baseline.add(dt)
				}
			}
		}()
	}

	start := time.Now()
	heavyDone := make(chan time.Duration, 1)
	go func() {
		time.Sleep(*warm)
		heavyOn.Store(true)
		heavyStart.Store(time.Now().UnixNano())
		t0 := time.Now()
		resp, err := heavyClient.Get(heavyURL)
		if err == nil {
			io.Copy(io.Discard, resp.Body)
			resp.Body.Close()
		}
		heavyDone <- time.Since(t0)
		heavyOn.Store(false)
	}()

	fmt.Printf("light load: c=%d duration=%s warmup=%s\n", *conc, *dur, *warm)
	hd := <-heavyDone
	if rest := *dur - time.Since(start); rest > 0 {
		time.Sleep(rest)
	}
	close(done)
	wg.Wait()

	baseline.report("baseline (no heavy)")
	during.report("during heavy")
	const win = 500.0 // ms; identical window on both sides
	reportLats(fmt.Sprintf("during heavy 0-%dms", int(win)), timedS.window(win))
	fmt.Printf("%-22s %v\n", "heavy request", hd.Round(time.Millisecond))
	fmt.Printf("%-22s %.0f\n", "during light req/s", float64(len(during.ms))/hd.Seconds())
	elapsed := time.Since(start).Seconds()
	fmt.Printf("%-22s %.0f\n", "light req/s (total)", float64(len(baseline.ms)+len(during.ms))/elapsed)
}
