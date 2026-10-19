// benchmark/vs-go/server.go — the Go side of the benchmark.
//
// The counterpart to server.ta: the same routes with the same work.
//
//	/light   -> a small fixed JSON document (byte-for-byte server.ta's)
//	/work/<n> -> n passes over a 1024-entry uint32 state array, in chunk-sized
//	             calls (the same call-per-chunk shape work.ta run() makes, so
//	             the Go runtime gets the same call boundaries to preempt at)
//	*        -> /light
//
// step() is a straight port of work.c work_step and run() of work.ta run():
// same state array, same seed per chunk, same recurrence, same fold, same
// bounds. The two differ only in what the compiler does with it — C vectorizes
// the element-parallel loop, Go emits scalar code.
//
// mix_bench.go / bench.go drive both servers with the same client, so the
// request sequence is identical on both sides.
//
// Usage: go run server.go <port>
package main

import (
	"fmt"
	"net/http"
	"os"
	"strconv"
	"strings"
)

// body is byte-for-byte the document server.ta serves.
const body = `{"message":"hello","ok":true}`

// Same constants as server.ta.
const (
	chunk     = 50 // passes per step() call — the chunking knob, same value
	maxPasses = 2_000_000
	ewN       = 1024
)

// Sink defeats dead-code elimination of the kernel loop: the response does not
// depend on the result, so without it the Go compiler would compute nothing
// and this side of the benchmark would be a lie. The TA side needs no Sink —
// the VM cannot eliminate a call into a native module.
var Sink uint64

// step is the same seed + passes over the same state array as work.c.
func step(seed, passes int64) uint64 {
	if passes < 0 {
		passes = 0
	}
	var o [ewN]uint32
	s := uint32(seed)*2654435761 + 1
	for j := 0; j < ewN; j++ {
		s = s*1664525 + 1013904223
		o[j] = s
	}
	for p := int64(0); p < passes; p++ {
		for j := 0; j < ewN; j++ {
			o[j] = o[j]*1664525 + 1013904223
		}
	}
	h := uint64(0x9E3779B97F4A7C15) + uint64(seed) + uint64(passes)
	for j := 0; j < ewN; j++ {
		h += uint64(o[j])
	}
	h = (h ^ (h >> 29)) * 0xff51afd7ed558ccd
	return h & 0x7FFFFFFFFFFF
}

// run is work.ta run(): total passes as total/chunk step() calls, each chunk
// seeded with seed+i so no call depends on another.
func run(seed, total, chunk int64) uint64 {
	if chunk < 1 {
		return ^uint64(0) // -1, as the TA side returns
	}
	var acc uint64
	for i := int64(0); i < total; i += chunk {
		n := chunk
		if i+chunk > total {
			n = total - i
		}
		acc += step(seed+i, n)
	}
	return acc
}

func main() {
	port := "8099"
	if len(os.Args) > 1 {
		port = os.Args[1]
	}
	light := func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(body))
	}
		http.HandleFunc("/work/", func(w http.ResponseWriter, r *http.Request) {
		n, err := strconv.ParseInt(strings.TrimPrefix(r.URL.Path, "/work/"), 10, 64)
		if err != nil || n <= 0 || n > maxPasses {
			// 400, not a silently smaller request: same as server.ta.
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusBadRequest)
			w.Write([]byte(`{"error":"size out of range"}`))
			return
		}
		Sink = run(n, n, chunk)
		light(w, r)
	})
	http.HandleFunc("/light", light)
	http.HandleFunc("/", light)
	fmt.Println("listening on", port)
	if err := http.ListenAndServe("127.0.0.1:"+port, nil); err != nil {
		fmt.Fprintln(os.Stderr, "listen failed:", err)
		os.Exit(1)
	}
}