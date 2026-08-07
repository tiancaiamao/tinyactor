// gobench — Minimal static file server (Go), for performance comparison
// against the TinyActor server (lib/serve.ta).
//
// Usage:
//   go run ./gobench -port 8124 -root ./lib
//
// Serves files under -root with directory index.html fallback (like the
// TA server). 404 for missing files.

package main

import (
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
	"path/filepath"
)

func main() {
	port := flag.String("port", "8124", "listen port")
	root := flag.String("root", ".", "static root directory")
	flag.Parse()

	abs, err := filepath.Abs(*root)
	if err != nil {
		log.Fatal(err)
	}
	if fi, err := os.Stat(abs); err != nil || !fi.IsDir() {
		log.Fatalf("root %s is not a directory", abs)
	}

	// Directory index: serve index.html inside the dir handler.
	fileServer := http.FileServer(http.Dir(abs))
	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		// mirror TA server: extensionless path falls back to path.html
		p := filepath.Clean(r.URL.Path)
		full := filepath.Join(abs, p)
		if fi, err := os.Stat(full); err == nil && fi.IsDir() {
			fileServer.ServeHTTP(w, r)
			return
		}
		if _, err := os.Stat(full + ".html"); err == nil {
			r.URL.Path = p + ".html"
		}
		fileServer.ServeHTTP(w, r)
	})

	addr := ":" + *port
	fmt.Printf("gobench serving %s on %s\n", abs, addr)
	log.Fatal(http.ListenAndServe(addr, nil))
}