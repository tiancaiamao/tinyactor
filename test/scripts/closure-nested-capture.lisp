;; Test: closure-nested-capture
;; Purpose: Three-level nested closure capturing different variables at
;;          each level. Verifies closure free-vars are correctly copied
;;          during GC and deep copy (spawn with closure).
;; Expected output: 6

(define (f x)
  (lambda (y)
    (lambda (z)
      (+ x (+ y z)))))

(print (((f 1) 2) 3))