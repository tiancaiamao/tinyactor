;; concurrent_test.lisp — proves actor suspend/resume concurrency
;;
;; An echo server listens on port 8092. Five client actors are spawned
;; at the same time; each connects, sends a unique message, reads back
;; the echo, and verifies the reply matches what it sent.
;;
;; Because net.accept and net.read block (the VM suspends the actor on
;; EAGAIN via vm_yield() and resumes it via poll),
;; multiple actors are suspended on I/O simultaneously with no busy-loop.
;; The main actor collects 5 results and prints "ALL PASS".
;;
;; The server accept loop is BOUNDED to exactly 5 connections, then it
;; closes the listen socket and exits — this is what lets the VM stop
;; (the scheduler only halts once no actor is waiting on I/O). This
;; makes the scenario a finite, self-contained test rather than a
;; forever-running server.
;;
;; Expected output: ALL PASS

(import "net")

;; ---- Server side -------------------------------------------------------

;; Per-connection server actor: read once, echo back, then close.
;; net.read blocks this actor until data arrives (VM suspends/resumes).
(define (handle-client fd)
  (let data (net.read fd))
  (match data
    ('eof (net.close fd))
    (_ (net.write fd data) (net.close fd))))

;; Accept exactly `remaining` connections, spawning one handler actor
;; per connection. net.accept blocks until a connection arrives. After
;; the last connection the listen socket is closed and this actor exits.
(define (accept-loop server-fd remaining)
  (if (= remaining 0)
      (net.close server-fd)
      (begin
        (let client-fd (net.accept server-fd))
        (spawn (lambda () (handle-client client-fd)))
        (accept-loop server-fd (- remaining 1)))))

;; ---- Client side -------------------------------------------------------

;; One client actor: connect, send its unique message, read the echo,
;; verify it matches, then report ok/fail to the collector.
(define (run-client port msg collector)
  (let fd (net.connect "127.0.0.1" port))
  (if (= fd -1)
      (send collector 'fail)
      (begin
        (net.write fd msg)
        (let reply (net.read fd 4096))
        (net.close fd)
        (if (string-eq reply msg)
            (send collector 'ok)
            (send collector 'fail)))))

;; ---- Result collector --------------------------------------------------

;; Wait for `remaining` results, tallying ok/fail, then print verdict.
(define (collect remaining ok-count fail-count)
  (if (= remaining 0)
      (if (= fail-count 0)
          (print "ALL PASS")
          (print "FAIL"))
      (match (recv)
        ('ok   (collect (- remaining 1) (+ ok-count 1) fail-count))
        ('fail (collect (- remaining 1) ok-count (+ fail-count 1))))))

;; ---- Driver ------------------------------------------------------------

(define (main)
  (let server-fd (net.listen 8092))
  (if (= server-fd -1)
      (print "FAIL: could not listen on 8092")
      (begin
        ;; Start the echo server (accepts exactly 5 connections)
        (spawn (lambda () (accept-loop server-fd 5)))
        (let me (self))
        ;; Launch five clients simultaneously, each with a unique message.
        ;; Several of these will be suspended on I/O at the same time.
        (spawn (lambda () (run-client 8092 "client-zero" me)))
        (spawn (lambda () (run-client 8092 "client-one" me)))
        (spawn (lambda () (run-client 8092 "client-two" me)))
        (spawn (lambda () (run-client 8092 "client-three" me)))
        (spawn (lambda () (run-client 8092 "client-four" me)))
        (collect 5 0 0))))