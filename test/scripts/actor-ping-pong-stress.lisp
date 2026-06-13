;; Test: actor-ping-pong-stress
;; Purpose: 1000 ping-pong messages between two actors. Allocates
;;          pairs for every message, exercising GC during heavy
;;          message passing. Uses alternating send/recv pattern.
;; Expected output: done

(define (ping n pong-pid)
  (if (= n 0)
      (begin
        (send pong-pid 'stop)
        (print "done"))
      (begin
        (send pong-pid (cons 'ping (self)))
        (match (recv)
          ('pong (ping (- n 1) pong-pid))))))

(define (pong)
  (match (recv)
    ('stop 'ok)
    (('ping . sender)
     (send sender 'pong)
     (pong))))

(define (main)
  (let pong-pid (spawn 'pong))
  (spawn (lambda () (ping 1000 pong-pid)))
  (let ref (monitor pong-pid))
  (recv))