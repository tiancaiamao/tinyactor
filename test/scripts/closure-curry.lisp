;; Test: closure-curry
;; Purpose: Partial application through currying. Exercises closure
;;          creation with 0, 1, 2 args. Inspired by cora's curry.cora,
;;          curry-as-arg.cora, curry-partial.cora.
;; Expected output: 5

(define (add a b)
  (+ a b))

(define (curry f x)
  (lambda (y) (f x y)))

(print ((curry add 3) 2))