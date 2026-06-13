;; Test: list-map
;; Purpose: Map a function over a list, creating a new list.
;;          Exercises closure application over list elements.
;;          Note: build 5 creates (5 4 3 2 1), map (+1) gives (6 5 4 3 2).
;; Expected output: (6 5 4 3 2)

(define (map f lst)
  (if (null? lst)
      'nil
      (cons (f (car lst)) (map f (cdr lst)))))

(define (build n)
  (if (= n 0)
      'nil
      (cons n (build (- n 1)))))

(print (map (lambda (x) (+ x 1)) (build 5)))