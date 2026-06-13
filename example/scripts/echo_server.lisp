;; echo_server.lisp — TCP Echo Server for TinyActor VM
(import "net")

(define (handle-client fd)
  (let data (net.read fd))
  (match data
    ('eof
     (net.close fd))
    ('would-block
     (handle-client fd))
    (_
     (net.write fd data)
     (handle-client fd)))))

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
  (let server-fd (net.listen 8090))
  (if (= server-fd -1)
      (print "failed to listen on port 8090")
      (begin
        (print "echo server listening on port 8090")
        (accept-loop server-fd))))