// The Go side of the benchmark: server.ta's routes with server.ta's work.
// Usage: go run server.go <port>
package main

import (
	"fmt"
	"net/http"
	"os"
	"strconv"
	"strings"
)

const body = `{"message":"hello","ok":true}`

// Same constants as server.ta.
const (
	chunk     = 50
	maxPasses = 2_000_000
	ewN       = 1024
)

// The response does not depend on the result, so without a Sink the Go
// compiler eliminates the kernel loop. The TA side needs no such thing.
var Sink uint64

// step is work.c work_step: same array, same recurrence, same fold.
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

// run is work.ta run(): total passes as total/chunk step() calls.
func run(seed, total, chunk int64) uint64 {
	if chunk < 1 {
		return ^uint64(0)
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
	json := func(w http.ResponseWriter, code int, s string) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(code)
		w.Write([]byte(s))
	}
	light := func(w http.ResponseWriter, r *http.Request) { json(w, 200, body) }

	http.HandleFunc("/work/", func(w http.ResponseWriter, r *http.Request) {
		n, err := strconv.ParseInt(strings.TrimPrefix(r.URL.Path, "/work/"), 10, 64)
		if err != nil || n <= 0 || n > maxPasses {
			json(w, http.StatusBadRequest, `{"error":"size out of range"}`)
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