;; Test: gc-deep-list
;; Purpose: Build a long list, reverse it, then compute its length.
;;          Forces GC as heap fills with pairs. Tests tail-recursive
;;          list operations under memory pressure.
;; Expected output: 2000

(define (build-list-helper n acc)
  (if (= n 0)
      acc
      (build-list-helper (- n 1) (cons n acc))))

(define (build-list n)
  (build-list-helper n 'nil))

(define (reverse-helper acc lst)
  (if (null? lst)
      acc
      (reverse-helper (cons (car lst) acc) (cdr lst))))

(define (reverse lst)
  (reverse-helper 'nil lst))

(define (length-helper lst acc)
  (if (null? lst)
      acc
      (length-helper (cdr lst) (+ acc 1))))

(define (length lst)
  (length-helper lst 0))

(print (length (reverse (build-list 2000))))