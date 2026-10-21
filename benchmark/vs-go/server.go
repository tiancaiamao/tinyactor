// The Go side of the benchmark: server.ta's service, in Go.
//   ./go-server 8099
//
// The service has four endpoints and no parameters — the workload is
// part of the protocol, so both sides do exactly the same work:
//
//	POST /light     -> {"bytes":65536,"sha256":"<hex>"}    (64KB digest)
//	POST /heavy     -> {"bytes":16777216,"sha256":"<hex>"}  (16MB digest)
//	POST /delay1ms  -> {"delay_ms":1}
//	POST /fairness  -> {"counts":[c0,...,c31]}
//
// The kernel is the Go standard library's crypto/sha256 — what a Go
// service would actually use — over the same synthetic document, hashed in
// the same 64KB blocks server.ta uses. The digests match, so what is left
// to compare is the runtime.
//
// The TinyActor side reaches OpenSSL's SHA-256 through the standard library
// module lib/sha256.ta. The two sides therefore run their own language's
// SHA-256, which is the honest comparison; the README measures the two
// kernels separately so the reader can tell kernel cost from runtime cost.
//
// /delay1ms is the latency-sensitive endpoint — the one tiancaiamao/sched
// is about. It sleeps for a millisecond and answers; everything the client
// sees above that millisecond is the scheduler's wake-up latency, and it is
// what a CPU-dense request in flight does to it.
//
// /fairness is the scheduler's fairness case: 32 CPU-bound goroutines,
// each spinning until a 3s deadline, and the per-worker iteration counts
// come back in the answer. The service does the work; the client computes
// the deviation (bench.go -fairness), so both sides are measured the same
// way. The TA side of the protocol is benchmark/vs-go/fairness.ta.
package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"sync"
	"time"
)

// The synthetic document's seed — the same bytes server.ta hashes.
const seed = "tinyactor-benchmark-payload-seed-v1-0123456789abcdefghijklmnopxy"

// The document is hashed this many bytes per write.
const blockSize = 65536

// The two payload sizes of the protocol. The client picks the endpoint,
// not the size — the sizes are part of the service, so both sides hash
// the same bytes.
const (
	lightBytes = 65536
	heavyBytes = 16777216
)

// The block of the document: the seed repeated, a whole number of times.
var block = buildBlock()

func buildBlock() []byte {
	b := make([]byte, 0, blockSize)
	for len(b) < blockSize {
		b = append(b, seed...)
	}
	return b
}

type response struct {
	Bytes  int64  `json:"bytes"`
	SHA256 string `json:"sha256"`
}

// delayResponse: the answer of the latency-sensitive endpoint. The
// tinyactor side writes the same bytes, so the client can check that both
// services did the same thing.
type delayResponse struct {
	DelayMS int `json:"delay_ms"`
}

// The fairness protocol's shape: 32 CPU-bound workers, a 3s window. The
// shape is part of the protocol, so both sides run the same one.
const (
	fairnessWorkers = 32
	fairnessWindow  = 3 * time.Second
)

// fairnessResponse: the per-worker iteration counts. The client computes
// the deviation from them — the service only does the work.
type fairnessResponse struct {
	Counts []int64 `json:"counts"`
}

// fairnessCounts: N goroutines, each spinning until the deadline and
// counting iterations. The one that gets more CPU does more iterations, so
// the spread of the counts is the unfairness. The goroutines never yield
// voluntarily — the runtime has to preempt them — so the difference
// between the two sides is the scheduler, not the work.
func fairnessCounts(workers int, window time.Duration) []int64 {
	deadline := time.Now().Add(window)
	counts := make([]int64, workers)

	var wg sync.WaitGroup
	for i := 0; i < workers; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			var c int64
			for time.Now().Before(deadline) {
				c++
			}
			counts[i] = c
		}(i)
	}
	wg.Wait()
	return counts
}

// digest: the SHA-256 of the first n bytes of the document, hashed in
// blockSize-byte chunks — server.ta's loop, call for call.
func digest(n int64) string {
	h := sha256.New()
	for done := int64(0); done < n; {
		take := int64(blockSize)
		if n-done < take {
			take = n - done
		}
		h.Write(block[:take])
		done += take
	}
	return hex.EncodeToString(h.Sum(nil))
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	body, err := json.Marshal(v)
	if err != nil {
		body = []byte(`{"error":"cannot encode"}`)
		code = 500
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	w.Write(body)
}

func main() {
	port := "8099"
	if len(os.Args) > 1 {
		port = os.Args[1]
	}

	// /light: hash the first 64KB. The digest is computed per request —
	// memoizing it would make the endpoint free and the comparison a lie.
	http.HandleFunc("/light", func(w http.ResponseWriter, r *http.Request) {
		io.Copy(io.Discard, r.Body)
		writeJSON(w, http.StatusOK, response{Bytes: lightBytes, SHA256: digest(lightBytes)})
	})

	// /heavy: hash the first 16MB, in the same 64KB blocks.
	http.HandleFunc("/heavy", func(w http.ResponseWriter, r *http.Request) {
		io.Copy(io.Discard, r.Body)
		writeJSON(w, http.StatusOK, response{Bytes: heavyBytes, SHA256: digest(heavyBytes)})
	})

	// /delay1ms: the latency-sensitive endpoint. The goroutine sleeps on a
	// timer and is then woken up — if every P is busy with a CPU-dense
	// request, the wake-up waits for the runtime to hand it a P, and the
	// client sees that wait on top of the millisecond.
	http.HandleFunc("/delay1ms", func(w http.ResponseWriter, r *http.Request) {
		io.Copy(io.Discard, r.Body)
		time.Sleep(time.Millisecond)
		writeJSON(w, http.StatusOK, delayResponse{DelayMS: 1})
	})

	// /fairness: the scheduler's fairness case. The goroutines are spawned
	// from the handler and the handler waits for them, so the window is
	// the same one the standalone program used to measure.
	http.HandleFunc("/fairness", func(w http.ResponseWriter, r *http.Request) {
		io.Copy(io.Discard, r.Body)
		writeJSON(w, http.StatusOK, fairnessResponse{
			Counts: fairnessCounts(fairnessWorkers, fairnessWindow),
		})
	})

	fmt.Println("listening on", port)
	if err := http.ListenAndServe("127.0.0.1:"+port, nil); err != nil {
		fmt.Fprintln(os.Stderr, "listen failed:", err)
		os.Exit(1)
	}
}