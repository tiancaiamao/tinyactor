;; Test: tail-call-deep
;; Purpose: Tail-call with 5M iterations. Verifies TCO prevents stack
;;          overflow and that GC doesn't break tail-call chain.
;; Expected output: 5000000

(define (sum r i)
  (if (= i 0)
      r
      (sum (+ r 1) (- i 1))))

(print (sum 0 5000000))