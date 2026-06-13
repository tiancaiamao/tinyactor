;; Test: closure-send-to-other-process
;; Purpose: Create a closure, spawn a process with it, verify the
;;          spawned process can call the closure with correct captured
;;          values. Tests deep copy of closures across process boundaries.
;; Expected output: 15

(define (make-adder n)
  (lambda (x) (+ x n)))

(define (worker adder)
  (send (self) (adder 10))
  (let result (recv))
  (print result))

(define (main)
  (let adder (make-adder 5))
  (let pid (spawn (lambda () (worker adder))))
  (let ref (monitor pid))
  (recv))