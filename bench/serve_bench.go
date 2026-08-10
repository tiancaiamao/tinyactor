// bench/serve_bench.go — HTTP load generator for lib/serve.ta (and any HTTP server).
//
// Usage:
//   go run bench/serve_bench.go -url http://127.0.0.1:8099/ -c 50 -n 5000
//   go run bench/serve_bench.go -url http://127.0.0.1:8099/style.css -c 20 -d 10s
//
// Serve uses one connection per request (no keep-alive), so the default
// DisableKeepAlives mirrors real behavior. -k enables keep-alive reuse.
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
	url         = flag.String("url", "http://127.0.0.1:8099/", "target URL")
	concurrency = flag.Int("c", 20, "number of concurrent workers")
	requests    = flag.Int("n", 2000, "total requests (ignored if -d is set)")
	duration    = flag.Duration("d", 0, "benchmark duration (e.g. 10s) instead of fixed count")
	keepalive   = flag.Bool("k", false, "reuse connections (serve.ta closes per request; default off)")
)

func main() {
	flag.Parse()
	if *requests <= 0 && *duration <= 0 {
		*requests = 2000
	}

	client := &http.Client{Transport: &http.Transport{DisableKeepAlives: !*keepalive}}
	var total, failed, bytes int64
	var mu sync.Mutex
	latencies := make([]time.Duration, 0, 1<<16)

	var wg sync.WaitGroup
	var next atomic.Int64
	start := time.Now()
	var deadline time.Time
	if *duration > 0 {
		deadline = start.Add(*duration)
	}

	worker := func() {
		defer wg.Done()
		for {
			if *duration > 0 {
				if time.Now().After(deadline) {
					return
				}
			} else if next.Add(1) > int64(*requests) {
				return
			}
			t0 := time.Now()
			resp, err := client.Get(*url)
			lat := time.Since(t0)
			mu.Lock()
			latencies = append(latencies, lat)
			mu.Unlock()
			if err != nil {
				atomic.AddInt64(&failed, 1)
				continue
			}
			n, _ := io.Copy(io.Discard, resp.Body)
			resp.Body.Close()
			atomic.AddInt64(&bytes, n)
			atomic.AddInt64(&total, 1)
			if resp.StatusCode >= 400 {
				atomic.AddInt64(&failed, 1)
			}
		}
	}

	for i := 0; i < *concurrency; i++ {
		wg.Add(1)
		go worker()
	}
	wg.Wait()
	elapsed := time.Since(start)

	mu.Lock()
	sort.Slice(latencies, func(i, j int) bool { return latencies[i] < latencies[j] })
	n := len(latencies)
	var sum time.Duration
	for _, l := range latencies {
		sum += l
	}
	pct := func(p float64) time.Duration {
		if n == 0 {
			return 0
		}
		return latencies[int(p*float64(n-1))]
	}
	mu.Unlock()

	fmt.Printf("target      : %s\n", *url)
	fmt.Printf("concurrency : %d  keep-alive: %v\n", *concurrency, *keepalive)
	fmt.Printf("requests    : %d ok, %d failed\n", total, failed)
	fmt.Printf("duration    : %s\n", elapsed.Round(time.Millisecond))
	if elapsed.Seconds() > 0 && total > 0 {
		fmt.Printf("throughput  : %.0f req/s\n", float64(total)/elapsed.Seconds())
		fmt.Printf("bandwidth   : %.2f MB/s\n", float64(bytes)/1e6/elapsed.Seconds())
	}
	if n > 0 {
		fmt.Printf("latency     : avg %s  p50 %s  p90 %s  p99 %s  max %s\n",
			(sum/time.Duration(n)).Round(time.Microsecond),
			pct(0.50), pct(0.90), pct(0.99), latencies[n-1])
	}
}