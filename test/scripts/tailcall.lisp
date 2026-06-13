;; Phase 1 test: tail call optimization
;; Expected output: done
;; Should NOT crash with stack overflow

(define (loop n)
  (if (= n 0)
      (print "done")
      (loop (- n 1))))

(loop 1000000)