;; Test: list-iterate-sum
;; Purpose: Build a list and sum its elements recursively.
;;          Exercises cons/car/cdr traversal under GC pressure.
;; Expected output: 5050

(define (sum-list lst acc)
  (if (null? lst)
      acc
      (sum-list (cdr lst) (+ acc (car lst)))))

(define (build n)
  (if (= n 0)
      'nil
      (cons n (build (- n 1)))))

(print (sum-list (build 100) 0))