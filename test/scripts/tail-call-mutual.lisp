;; Test: tail-call-mutual
;; Purpose: Mutual tail recursion between two functions. Verifies
;;          TCO works across function boundaries (A calls B in tail
;;          position, B calls A in tail position).
;; Expected output: true

(define (even? n)
  (if (= n 0)
      'true
      (odd? (- n 1))))

(define (odd? n)
  (if (= n 0)
      'false
      (even? (- n 1))))

(print (even? 100000))