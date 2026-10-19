// benchmark/vs-go/server.go — mixed-load HTTP service (Go side).
//
// The counterpart to server.ta: the same two routes with the same work.
//
//	/light -> a small fixed JSON document (byte-for-byte server.ta's)
//	/heavy -> heavyRounds rounds of the same integer mix, in heavyChunk-sized
//	          calls (the same call-per-chunk shape compute.burn runs, so the
//	          Go runtime gets the same call boundaries to preempt at)
//	*      -> /light
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
	heavyRounds = 400_000_000
	heavyChunk  = 2000
)

// Sink defeats dead-code elimination of the mix loop.
var Sink uint64

// kernel is the same 64-bit integer mix as lib/compute.c compute_kernel.
func kernel(n int64) uint64 {
	x := uint64(n)*0x9E3779B97F4A7C15 + 1
	for i := int64(0); i < n; i++ {
		x ^= x >> 33
		x *= 0xff51afd7ed558ccd
		x ^= x >> 29
		x += uint64(i)
	}
	return x & 0x7FFFFFFFFFFF
}

func heavyWork() {
	var acc uint64
	for i := int64(0); i < heavyRounds; i += heavyChunk {
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