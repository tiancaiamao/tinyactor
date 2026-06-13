;; Test: list-reverse
;; Purpose: Build and reverse a list. Exercises cons/car/cdr in
;;          a tight recursive loop with heap allocation.
;;          Note: build 5 creates (5 4 3 2 1), reverse gives (1 2 3 4 5).
;; Expected output: (1 2 3 4 5)

(define (reverse-helper acc lst)
  (if (null? lst)
      acc
      (reverse-helper (cons (car lst) acc) (cdr lst))))

(define (reverse lst)
  (reverse-helper 'nil lst))

(define (build n)
  (if (= n 0)
      'nil
      (cons n (build (- n 1)))))

(print (reverse (build 5)))