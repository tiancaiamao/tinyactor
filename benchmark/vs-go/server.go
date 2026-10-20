// The Go side of the benchmark: server.ta's service, server.ta's kernel.
//   ./go-server 8099
//
// The kernel is OpenSSL's EVP SHA-256 — the same library the TinyActor side
// reaches through lib/sha256.c — over the same synthetic document, hashed
// in the same 16KB blocks. The digests match, so what is left to compare
// is the runtime.
//
// The cgo flags for OpenSSL come from the environment (CGO_CFLAGS /
// CGO_LDFLAGS); `make` in this directory bakes them in.
package main

/*
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>

static EVP_MD_CTX *sha_new(void) {
	EVP_MD_CTX *c = EVP_MD_CTX_new();
	if (c != NULL && EVP_DigestInit_ex(c, EVP_sha256(), NULL) != 1) {
		EVP_MD_CTX_free(c);
		c = NULL;
	}
	return c;
}

static void sha_free(EVP_MD_CTX *c) {
	if (c != NULL)
		EVP_MD_CTX_free(c);
}

static int sha_update(EVP_MD_CTX *c, const unsigned char *p, size_t n) {
	return EVP_DigestUpdate(c, p, n) == 1;
}

static int sha_final(EVP_MD_CTX *c, char *out) {
	static const char digits[] = "0123456789abcdef";
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned int len = 0;
	if (EVP_DigestFinal_ex(c, md, &len) != 1 || len != 32)
		return 0;
	for (unsigned int i = 0; i < len; i++) {
		out[2 * i] = digits[md[i] >> 4];
		out[2 * i + 1] = digits[md[i] & 0xf];
	}
	out[64] = '\0';
	return 1;
}
*/
import "C"

import (
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"unsafe"
)

// The synthetic document's seed — the same bytes server.ta hashes.
const seed = "tinyactor-benchmark-payload-seed-v1-0123456789abcdefghijklmnopxy"

// The document is hashed this many bytes per native call.
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
	ctx := C.sha_new()
	if ctx == nil {
		return ""
	}
	defer C.sha_free(ctx)
	for done := int64(0); done < n; {
		take := int64(blockSize)
		if n-done < take {
			take = n - done
		}
		if C.sha_update(ctx, (*C.uchar)(unsafe.Pointer(&block[0])), C.size_t(take)) == 0 {
			return ""
		}
		done += take
	}
	out := make([]byte, 65)
	if C.sha_final(ctx, (*C.char)(unsafe.Pointer(&out[0]))) == 0 {
		return ""
	}
	return string(out[:64])
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