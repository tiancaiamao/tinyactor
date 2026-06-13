;; Test: gc-cross-reference
;; Purpose: Closures capture pair references. Verifies GC correctly
;;          traces the full object graph — closures that reference
;;          heap pairs must keep those pairs alive through GC cycles.
;; Expected output: 42

(define (make-clos-inner pair)
  (lambda () (car pair)))

(define (make-clos n)
  (make-clos-inner (cons n (+ n 1))))

(define (churn n)
  (if (= n 0)
      'nil
      (begin
        (make-clos n)
        (churn (- n 1)))))

(define (test)
  (let f (make-clos 42))
  (churn 500)
  (f))

(print (test))