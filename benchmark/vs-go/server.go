// The Go side of the benchmark: server.ta's service, in Go.
//   ./go-server 8099
//
// The kernel is the Go standard library's crypto/sha256 — what a Go
// service would actually use — over the same synthetic document, hashed in
// the same 16KB blocks server.ta uses. The digests match, so what is left
// to compare is the runtime.
//
// The TinyActor side reaches OpenSSL's EVP through the standard library
// module lib/sha256.ta. The two sides therefore run their own language's
// SHA-256, which is the honest comparison; the README measures the two
// kernels separately so the reader can tell kernel cost from runtime cost.
package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net/http"
	"os"
)

// The synthetic document's seed — the same bytes server.ta hashes.
const seed = "tinyactor-benchmark-payload-seed-v1-0123456789abcdefghijklmnopxy"

// The document is hashed this many bytes per write.
const blockSize = 16384

// The largest request the service accepts.
const maxBytes = 67108864

// The block of the document: the seed repeated, a whole number of times.
var block = buildBlock()

func buildBlock() []byte {
	b := make([]byte, 0, blockSize)
	for len(b) < blockSize {
		b = append(b, seed...)
	}
	return b
}

type request struct {
	Bytes *int64 `json:"bytes"`
}

type response struct {
	Bytes  int64  `json:"bytes"`
	SHA256 string `json:"sha256"`
}

type errorResponse struct {
	Error string `json:"error"`
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

	http.HandleFunc("/digest", func(w http.ResponseWriter, r *http.Request) {
		var q request
		if err := json.NewDecoder(r.Body).Decode(&q); err != nil || q.Bytes == nil {
			writeJSON(w, http.StatusBadRequest, errorResponse{Error: "body is not JSON"})
			return
		}
		n := *q.Bytes
		if n < 0 || n > maxBytes {
			writeJSON(w, http.StatusBadRequest, errorResponse{Error: "bytes out of range"})
			return
		}
		writeJSON(w, http.StatusOK, response{Bytes: n, SHA256: digest(n)})
	})

	fmt.Println("listening on", port)
	if err := http.ListenAndServe("127.0.0.1:"+port, nil); err != nil {
		fmt.Fprintln(os.Stderr, "listen failed:", err)
		os.Exit(1)
	}
}