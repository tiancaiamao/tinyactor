;; Test: tail-call-do-in-tail
;; Purpose: (begin ...) in tail position should still be optimized
;;          as a tail call. The inner expression of begin in tail
;;          position is the actual tail. Counts down from 1000000 to 0.
;; Expected output: 0

(define (count n)
  (if (= n 0)
      n
      (begin
        (count (- n 1)))))

(print (count 1000000))