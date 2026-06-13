# Task 5: HTTP Server Example

Create a working HTTP server that demonstrates TinyActor handling real-world protocol with actor concurrency.

## What to Build

### `example/http_server.c`
C main program that:
1. Creates TinyActor VM
2. Registers net module + HTTP helper functions
3. Loads and runs http_server.lisp

### HTTP Helper C Functions (register as "http" module)
These handle the low-level HTTP parsing/formatting that's tedious in Lisp:

1. **http.parse_request(data_string)** — Parse raw HTTP request
   - Returns a pair: `(method . path)` or a more detailed structure
   - e.g., "GET /hello HTTP/1.1\r\n..." → `("GET" . "/hello")`

2. **http.response(status_code, content_type, body_string)** — Build HTTP response string
   - e.g., `(http.response 200 "text/html" "Hello")` → "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 5\r\n\r\nHello"

3. **http.header(name, value)** — Build a single header line (optional helper)

### `example/scripts/http_server.lisp`
Script that:
1. Imports net and http modules
2. Starts TCP server on port 8080
3. Defines route handlers:
```lisp
(define (handle-request method path conn)
  (cond
    ((eq path "/")      (respond conn 200 "text/html" "<h1>Hello from TinyActor!</h1>"))
    ((eq path "/api")   (respond conn 200 "application/json" "{\"status\":\"ok\"}"))
    ((eq path "/time")  (respond conn 200 "text/plain" (get-time-str)))
    (else               (respond conn 404 "text/plain" "Not Found"))))
```
4. Each connection is handled by a separate actor (actor-per-connection)
5. Reads full HTTP request, parses it, dispatches to handler, sends response, closes

### `example/Makefile`
Add http_server target

## Verification
```bash
cd /Users/genius/project/tinyactor/example
make http_server
./http_server &
sleep 1
curl http://localhost:8080/         # → <h1>Hello from TinyActor!</h1>
curl http://localhost:8080/api      # → {"status":"ok"}
curl http://localhost:8080/time     # → current time
curl http://localhost:8080/nada     # → Not Found
kill %1
```

## Requirements
- At least 3 routes with different content
- Concurrent request handling (actor-per-connection)
- Proper HTTP response headers (Content-Type, Content-Length)
- Works with curl, browsers, and any HTTP client

## Files to Create
1. `example/http_server.c` (~60 lines)
2. `example/scripts/http_server.lisp` (~60 lines)
3. Possibly: `src/http.c` for HTTP helper functions (~80 lines) or inline in http_server.c

## Files to Modify
1. `example/Makefile` — add http_server target

## Estimated
- ~200 lines new code