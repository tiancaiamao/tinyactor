;; Test: gc-string-churn
;; Purpose: Allocate many strings via concat in a loop. Forces GC to
;;          copy HeapString objects repeatedly. Verifies no corruption.
;; Expected output: hello world

(define (churn n)
  (if (= n 0)
      "hello world"
      (begin
        (string-concat "prefix-" "mid-suffix")
        (string-concat (string-concat "x" "y") "z")
        (churn (- n 1)))))

(print (churn 500))