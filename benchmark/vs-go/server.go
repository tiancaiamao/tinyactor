// The Go side of the benchmark: server.ta's routes, server.ta's kernel.
//   go run server.go 8099
//
// The kernel is the SAME C code the TinyActor side calls (lib/libwork.dylib,
// built by `make` here): work_step_c(seed, passes) over the same 1024-entry
// array, chunked the same way, seeded the same way. A cgo call is atomic for
// the goroutine, so the chunk loop is what gives the Go scheduler a place to
// preempt — the same knob TA has.
package main

/*
#cgo LDFLAGS: -L${SRCDIR}/lib -lwork -Wl,-rpath,${SRCDIR}/lib
#include <stdint.h>
uint64_t work_step_c(uint64_t seed, int64_t passes);
*/
import "C"

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
)

// The response carries the checksum, so both servers can be checked to have
// computed the same thing from the same request.
var Sink int64

// run is server.ta's run: total passes as total/chunk native calls.
//
// The accumulator is truncated to 48 bits signed, because that is what TA's
// Int is: the two sides must fold the chunks the same way or their checksums
// differ for reasons that have nothing to do with the runtime.
func run(seed, total, chunk int64) int64 {
	if chunk < 1 {
		return -1
	}
	var acc int64
	for i := int64(0); i < total; i += chunk {
		n := chunk
		if i+chunk > total {
			n = total - i
		}
		acc = (acc + int64(C.work_step_c(C.uint64_t(seed+i), C.int64_t(n)))) << 16 >> 16
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
		json(w, 200, fmt.Sprintf(`{"sum":%d}`, Sink))
	})
	http.HandleFunc("/light", light)
	http.HandleFunc("/", light)
	fmt.Println("listening on", port)
	if err := http.ListenAndServe("127.0.0.1:"+port, nil); err != nil {
		fmt.Fprintln(os.Stderr, "listen failed:", err)
		os.Exit(1)
	}
}