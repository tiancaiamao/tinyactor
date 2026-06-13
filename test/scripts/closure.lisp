;; Phase 1 test: closure capturing variables
;; Expected output: 8

(define (adder n)
  (lambda (x) (+ x n)))

(print ((adder 5) 3))