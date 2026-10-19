// benchmark/vs-go/server.go — minimal HTTP JSON service (Go side).
//
// The Go counterpart to server.ta: one route returning the same fixed JSON
// document. The same client (serve_bench.go) load-tests both — see run.sh /
// `make benchmark-vs-go`.
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

func main() {
	port := "8099"
	if len(os.Args) > 1 {
		port = os.Args[1]
	}
	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(body))
	})
	fmt.Println("listening on", port)
	if err := http.ListenAndServe("127.0.0.1:"+port, nil); err != nil {
		fmt.Fprintln(os.Stderr, "listen failed:", err)
		os.Exit(1)
	}
}
