;; Phase 1 test: ping-pong actor message passing
;; Expected output: ping done\npong done\nall done

(define (ping n pong-pid)
  (if (= n 0)
      (begin
        (print "ping done")
        (send pong-pid 'stop))
      (begin
        (send pong-pid (cons 'ping (self)))
        (match (recv)
          ('pong (ping (- n 1) pong-pid))))))

(define (pong)
  (match (recv)
    ('stop (print "pong done"))
    (('ping . sender)
     (send sender 'pong)
     (pong))))

(define (main)
  (let pong-pid (spawn 'pong))
  (let ping-pid (spawn (lambda () (ping 100 pong-pid))))
  (let ref (monitor ping-pid))
  (match (recv)
    (('DOWN r pid reason)
     (print "all done"))))