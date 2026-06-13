;; Test: string-gc-stress
;; Purpose: Create many strings via concat in a loop. Forces GC to
;;          copy HeapString objects repeatedly. Verifies no corruption.
;; Expected output: done

(define (loop n)
  (if (= n 0)
      (print "done")
      (begin
        (string-concat "abc" "def")
        (string-concat (string-concat "x" "y") "z")
        (loop (- n 1)))))

(loop 5000)