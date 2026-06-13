;; Test: integration-fib-closure-actor
;; Purpose: Integration test combining fib computation, closure capture,
;;          and actor message passing in a single scenario. A worker
;;          computes fib(25) and sends the result back via actor message.
;; Expected output: 75025

(define (fib n)
  (if (< n 2)
      n
      (+ (fib (- n 1)) (fib (- n 2)))))

(define (worker parent)
  (send parent (fib 25)))

(define (main)
  (let me (self))
  (let pid (spawn (lambda () (worker me))))
  (let result (recv))
  (print result))