;; Phase 1 test: monitor and DOWN notification
;; Expected output: DOWN received

(define (worker)
  (/ 1 0))

(define (main)
  (let pid (spawn 'worker))
  (let ref (monitor pid))
  (match (recv)
    (('DOWN r p reason)
     (print "DOWN received"))))