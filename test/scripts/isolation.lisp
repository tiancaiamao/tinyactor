;; Phase 1 test: process isolation
;; One process crashes, another survives
;; Expected output: survived

(define (crasher)
  (/ 1 0))

(define (main)
  (spawn 'crasher)
  (send (self) 'hello)
  (recv)
  (print "survived"))