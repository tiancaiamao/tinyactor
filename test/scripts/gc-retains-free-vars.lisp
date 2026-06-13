;; Test: gc-retains-free-vars
;; Purpose: Closure's free variables must survive GC. This test creates
;;          a closure, churns the heap to trigger GC, then calls the
;;          closure and verifies captured values are intact.
;; Expected output: 42

(define (make-clos x)
  (lambda () x))

(define (churn n)
  (if (= n 0)
      'nil
      (begin
        (cons n (cons (+ n 1) 'nil))
        (churn (- n 1)))))

(define (test)
  (let f (make-clos 42))
  (churn 3000)
  (f))

(print (test))