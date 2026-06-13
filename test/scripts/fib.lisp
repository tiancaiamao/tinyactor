;; Phase 1 test: Fibonacci
;; Expected output: 832040

(define (fib n)
  (if (< n 2)
      n
      (+ (fib (- n 1)) (fib (- n 2)))))

(print (fib 30))