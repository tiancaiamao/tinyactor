// bench — Concurrent HTTP benchmark client (Go)
//
// Usage:
//   go run ./bench -port 8123 -path / -n 1000 -c 16
//   go run ./bench -port 8123 -path /typecheck.ta -n 1000 -c 16 -k=false
//
// Flags:
//   -port   server port          (default 8123)
//   -path   URL path             (default "/")
//   -n      total requests       (default 1000)
//   -c      concurrency          (default 16)
//   -k      keep-alive on/off    (default true; false = one conn per request)
//
// Prints total time, QPS, latency percentiles (avg/p50/p95/p99), errors
// and throughput. Errors are network/status failures (status != 200).

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

type stats struct {
	ok       int64
	err      int64
	bytes    int64
	lat      []time.Duration
	mu       sync.Mutex
}

func main() {
	port := flag.Int("port", 8123, "server port")
	path := flag.String("path", "/", "URL path")
	n := flag.Int("n", 1000, "total requests")
	c := flag.Int("c", 16, "concurrency")
	keepalive := flag.Bool("k", true, "keep-alive connections")
	flag.Parse()

	if *n <= 0 || *c <= 0 {
		fmt.Println("n and c must be > 0")
		return
	}

	url := fmt.Sprintf("http://127.0.0.1:%d%s", *port, *path)
	client := &http.Client{
		Transport: &http.Transport{
			DisableKeepAlives: !*keepalive,
		},
		Timeout: 30 * time.Second,
	}

	var s stats
	var wg sync.WaitGroup
	start := time.Now()

	per, extra := *n / *c, *n % *c
	for i := 0; i < *c; i++ {
		share := per
		if i == *c-1 {
			share += extra
		}
		wg.Add(1)
		go func(share int) {
			defer wg.Done()
			for j := 0; j < share; j++ {
				t0 := time.Now()
				resp, err := client.Get(url)
				d := time.Since(t0)
				s.mu.Lock()
				s.lat = append(s.lat, d)
				s.mu.Unlock()
				if err != nil {
					atomic.AddInt64(&s.err, 1)
					continue
				}
				body, _ := io.Copy(io.Discard, resp.Body)
				resp.Body.Close()
				atomic.AddInt64(&s.bytes, body)
				if resp.StatusCode == 200 {
					atomic.AddInt64(&s.ok, 1)
				} else {
					atomic.AddInt64(&s.err, 1)
				}
			}
		}(share)
	}
	wg.Wait()
	elapsed := time.Since(start)

	s.mu.Lock()
	lats := make([]time.Duration, len(s.lat))
	copy(lats, s.lat)
	s.mu.Unlock()
	sort.Slice(lats, func(a, b int) bool { return lats[a] < lats[b] })

	ms := func(d time.Duration) float64 { return float64(d) / float64(time.Millisecond) }
	pct := func(p float64) time.Duration {
		if len(lats) == 0 {
			return 0
		}
		i := int(p * float64(len(lats)) / 100)
		if i >= len(lats) {
			i = len(lats) - 1
		}
		return lats[i]
	}

	secs := elapsed.Seconds()
	qps := float64(s.ok) / secs
	mbs := float64(s.bytes) / secs / 1024 / 1024

	fmt.Printf("requests:  %d   concurrency: %d   keep-alive: %v\n", *n, *c, *keepalive)
	fmt.Printf("target:    %s\n", url)
	fmt.Printf("ok:        %d   err: %d\n", s.ok, s.err)
	fmt.Printf("total:     %.0f ms\n", elapsed.Seconds()*1000)
	fmt.Printf("qps:       %.0f\n", qps)
	fmt.Printf("latency:   avg %.2f ms | p50 %.2f | p95 %.2f | p99 %.2f\n",
		ms(avg(lats)), ms(pct(50)), ms(pct(95)), ms(pct(99)))
	fmt.Printf("throughput: %.1f MB/s\n", mbs)
}

func avg(l []time.Duration) time.Duration {
	if len(l) == 0 {
		return 0
	}
	var s time.Duration
	for _, d := range l {
		s += d
	}
	return s / time.Duration(len(l))
}