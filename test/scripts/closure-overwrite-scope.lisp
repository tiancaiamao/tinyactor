;; Test: closure-overwrite-scope
;; Purpose: Closure captures a variable that is later shadowed by let.
;;          Verifies the closure retains the original binding.
;; Expected output: 1

(define (test)
  (let x 1)
  (let f (lambda () x))
  (let x 99)
  (f))

(print (test))