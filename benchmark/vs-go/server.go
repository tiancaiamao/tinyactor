// benchmark/vs-go/server.go — mixed-load HTTP service (Go side).
//
// The counterpart to server.ta: the same two routes with the same work.
//
//	/light -> a small fixed JSON document (byte-for-byte server.ta's)
//	/heavy -> heavyPasses passes over a 1024-entry uint32 state array, in
//	          heavyChunk-sized calls (the same call-per-chunk shape
//	          compute.burn runs, so the Go runtime gets the same call
//	          boundaries to preempt at)
//	*      -> /light
//
// kernel() is a straight port of lib/compute.c compute_kernel: same state
// array, same recurrence, same fold. The two differ only in what the compiler
// does with it — C vectorizes the element-parallel loop, Go emits scalar code.
//
// fair.sh drives both servers with the same client.
//
// Usage: go run server.go <port>
package main

import (
	"fmt"
	"net/http"
	"os"
)

// body is byte-for-byte the document server.ta serves.
const body = `{"message":"hello","ok":true}`

// Same constants as server.ta.
const (
	heavyPasses = 10_000_000
	heavyChunk  = 50
	ewN         = 1024
)

// Sink defeats dead-code elimination of the kernel loop.
var Sink uint64

// kernel is the same pass over the same state array as lib/compute.c
// compute_kernel.
func kernel(passes int64) uint64 {
	var o [ewN]uint32
	for j := 0; j < ewN; j++ {
		o[j] = uint32(j)*0x9E3779B9 + 1
	}
	for p := int64(0); p < passes; p++ {
		for j := 0; j < ewN; j++ {
			o[j] = o[j]*1664525 + 1013904223
		}
	}
	h := uint64(0x9E3779B97F4A7C15) + uint64(passes)
	for j := 0; j < ewN; j++ {
		h += uint64(o[j])
	}
	h = (h ^ (h >> 29)) * 0xff51afd7ed558ccd
	return h & 0x7FFFFFFFFFFF
}

func heavyWork() {
	var acc uint64
	for i := int64(0); i < heavyPasses; i += heavyChunk {
		acc += kernel(heavyChunk)
	}
	Sink = acc
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
	http.HandleFunc("/light", light)
	http.HandleFunc("/heavy", func(w http.ResponseWriter, r *http.Request) {
		heavyWork()
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(body))
	})
	http.HandleFunc("/", light)
	fmt.Println("listening on", port)
	if err := http.ListenAndServe("127.0.0.1:"+port, nil); err != nil {
		fmt.Fprintln(os.Stderr, "listen failed:", err)
		os.Exit(1)
	}
}