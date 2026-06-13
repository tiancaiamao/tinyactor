;; Test: gc-closure-churn
;; Purpose: Allocate many closures with free variables, then discard them.
;;          Exercises GC's ability to copy HeapClosure objects and update
;;          references to captured values.
;; Expected output: 42

(define (make-closure x)
  (lambda (y)
    (lambda (z)
      (+ x (+ y z)))))

(define (churn n)
  (if (= n 0)
      'nil
      (begin
        (make-closure n)
        (churn (- n 1)))))

(churn 500)
(print (((make-closure 20) 10) 12))