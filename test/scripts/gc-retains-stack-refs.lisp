;; Test: gc-retains-stack-refs
;; Purpose: GC must correctly identify values on the evaluation stack
;;          as roots. This test creates temporary pairs that are on the
;;          stack when a GC could trigger, then uses them afterward.
;; Expected output: 30

(define (churn n)
  (if (= n 0)
      'nil
      (begin
        (cons n 'nil)
        (churn (- n 1)))))

(define (test)
  (let a (cons 10 20))
  (churn 2000)
  (+ (car a) (cdr a)))

(print (test))