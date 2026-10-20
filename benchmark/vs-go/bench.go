// The benchmark's client: draws each request's size 80/20 from a per-worker
// seeded PRNG, so both servers are handed the same sizes in the same order.
//   ./bench -url http://127.0.0.1:8099
//
// Every request is a JSON POST ({"bytes":N}); every response carries the
// digest of the N-byte document, so the two servers can be compared byte
// for byte. One request in VERIFY_EVERY is checked against a digest the
// client computes itself — the client is the only side not under test, so
// it is the one that can afford the extra hashing.
package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net/http"
	"sort"
	"sync"
	"sync/atomic"
	"time"
)

const seed = "tinyactor-benchmark-payload-seed-v1-0123456789abcdefghijklmnopxy"

// VERIFY_EVERY: how many responses are checked against a locally computed
// digest. The check hashes the whole document, so it is sampled.
const VERIFY_EVERY = 64

var (
	baseURL  = flag.String("url", "http://127.0.0.1:8099", "base URL")
	conc     = flag.Int("c", 16, "request concurrency")
	dur      = flag.Duration("d", 12*time.Second, "duration of the measured window")
	heavyPct = flag.Int("heavypct", 20, "percent of requests that are heavy")
	lightN   = flag.Int("lightn", 65536, "bytes in a light request")
	heavyN   = flag.Int("heavyn", 16777216, "bytes in a heavy request")
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

// expected: the digest of the first n bytes of the document — the seed
// repeated, truncated.
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

// draw is a per-worker LCG: worker i issues the same size sequence for
// both servers, so the two sides are handed identical work.
type draw struct{ s uint64 }

func (d *draw) next() uint64 {
	d.s = d.s*6364136223846793005 + 1442695040888963407
	return d.s >> 33
}

func main() {
	flag.Parse()

	tr := &http.Transport{MaxIdleConnsPerHost: 2 * *conc}
	client := &http.Client{Transport: tr, Timeout: 5 * time.Minute}

	var heavyInFlight atomic.Int64
	var seq atomic.Int64
	var badDigest atomic.Int64
	light, heavy := &bucket{}, &bucket{}

	post := func(n int) (time.Duration, error) {
		body := fmt.Sprintf(`{"bytes":%d}`, n)
		t0 := time.Now()
		resp, err := client.Post(*baseURL+"/digest", "application/json", bytes.NewReader([]byte(body)))
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
			if err := json.Unmarshal(raw, &r); err != nil || r.Bytes != int64(n) || r.SHA256 != expected(n) {
				badDigest.Add(1)
			}
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
					heavyInFlight.Add(1)
					dt, err := post(*heavyN)
					if err != nil {
						heavy.err()
					} else {
						heavy.add(dt)
					}
					heavyInFlight.Add(-1)
					continue
				}
				dt, err := post(*lightN)
				if err != nil {
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

	fmt.Printf("c=%d duration=%s  heavy=%d%% (%d bytes), light=%d%% (%d bytes)\n\n",
		*conc, *dur, *heavyPct, *heavyN, 100-*heavyPct, *lightN)
	light.report("light")
	heavy.report("heavy")
	fmt.Printf("%-14s light %.0f/s  heavy %.0f/s  total %.0f/s\n", "throughput",
		float64(light.count())/secs, float64(heavy.count())/secs,
		float64(light.count()+heavy.count())/secs)
	if bad := badDigest.Load(); bad > 0 {
		fmt.Printf("%-14s %d responses did not match the digest the client computed\n", "digest", bad)
	} else {
		fmt.Printf("%-14s every sampled response matched\n", "digest")
	}
}