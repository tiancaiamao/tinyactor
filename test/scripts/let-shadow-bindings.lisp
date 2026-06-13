;; Test: let-shadow-bindings
;; Purpose: Sequential let bindings where later bindings shadow earlier
;;          ones. Verifies correct scoping in the compiler.
;; Expected output: 456

(define (test)
  (let x 123)
  (let x 456)
  (print x))

(test)