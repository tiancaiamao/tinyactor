;; http_server.lisp — HTTP Server for TinyActor VM
(import "net")
(import "http")

;; Route handler: dispatch on parsed (method . path)
(define (handle-request conn parsed)
  (let method (car parsed))
  (let path (cdr parsed))
  (if (string-eq path "/")
      (respond conn 200 "text/html" "<h1>Hello from TinyActor!</h1>")
      (if (string-eq path "/api")
          (respond conn 200 "application/json" "{\"status\":\"ok\"}")
          (if (string-eq path "/time")
              (respond conn 200 "text/plain" "2025-01-01T00:00:00Z")
              (respond conn 404 "text/plain" "Not Found")))))

(define (respond conn status content-type body)
  (let resp (http.response status content-type body))
  (net.write conn resp)
  (net.close conn))

;; Per-connection actor: read request, parse, dispatch
(define (handle-client fd)
  (let data (net.read fd))
  (match data
    ('eof
     (net.close fd))
    ('would-block
     (handle-client fd))
    (_
     (let parsed (http.parse_request data))
     (match parsed
       ('nil
        (net.close fd))
       (_
        (handle-request fd parsed))))))

;; Accept loop: spawn an actor for each new connection
(define (accept-loop server-fd)
  (let result (net.accept server-fd))
  (match result
    (-1
     (print "accept error")
     (net.close server-fd))
    ('would-block
     (accept-loop server-fd))
    (_
     (let client-fd result)
     (spawn (lambda () (handle-client client-fd)))
     (accept-loop server-fd))))

(define (main)
  (let server-fd (net.listen 8080))
  (if (= server-fd -1)
      (print "failed to listen on port 8080")
      (begin
        (print "HTTP server listening on port 8080")
        (accept-loop server-fd))))