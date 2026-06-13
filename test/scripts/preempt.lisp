;; Phase 1 test: preemptive scheduling
;; One process runs infinite loop, another prints and exits
;; Expected output: ok (within a few seconds)

(define (infinite-loop)
  (infinite-loop))

(define (main)
  (spawn 'infinite-loop)
  (print "ok"))