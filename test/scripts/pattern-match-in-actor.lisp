;; Test: pattern-match-in-actor
;; Purpose: Actor uses pattern matching in its main recv loop.
;;          Tests match + recv integration under concurrent execution.
;; Expected output: pong\nping\ndone

(define (server)
  (match (recv)
    (('ping . from)
     (print "pong")
     (send from 'pong)
     (server))
    ('stop
     (print "done"))))

(define (client server-pid)
  (send server-pid (cons 'ping (self)))
  (match (recv)
    ('pong
     (print "ping")
     (send server-pid 'stop))))

(define (main)
  (let srv (spawn 'server))
  (spawn (lambda () (client srv)))
  (let ref (monitor srv))
  (recv))