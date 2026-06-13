;; echo_test.lisp — Integration test for TCP Echo Server
(import "net")

(define (handle-client fd)
  (let data (net.read fd))
  (match data
    ('eof (net.close fd))
    ('would-block (handle-client fd))
    (_ (net.write fd data) (net.close fd))))

(define (server-accept-loop server-fd)
  (let result (net.accept server-fd))
  (match result
    (-1 (net.close server-fd))
    ('would-block (server-accept-loop server-fd))
    (_
     (spawn (lambda () (handle-client result)))
     (server-accept-loop server-fd))))

(define (test-client port msg collector-pid)
  (let fd (net.connect "127.0.0.1" port))
  (if (= fd -1)
      (begin
        (print "connect failed")
        (send collector-pid 'fail))
      (begin
        (net.write fd msg)
        (let response (net.read fd 4096))
        (net.close fd)
        (if (string? response)
            (send collector-pid 'ok)
            (begin
              (print "bad response")
              (send collector-pid 'fail))))))

(define (collect n)
  (if (= n 0)
      (print "PASS")
      (match (recv)
        ('ok (collect (- n 1)))
        (_ (print "FAIL")))))

(define (main)
  (let server-fd (net.listen 8091))
  (spawn (lambda () (server-accept-loop server-fd)))
  (let me (self))
  (spawn (lambda () (test-client 8091 "hello" me)))
  (spawn (lambda () (test-client 8091 "world" me)))
  (spawn (lambda () (test-client 8091 "test" me)))
  (collect 3))