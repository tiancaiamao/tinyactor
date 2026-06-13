;; Test: gc-multi-process-stress
;; Purpose: Spawn many processes, each allocating pairs heavily.
;;          Verifies per-process GC isolation.
;; Expected output: all done

(define (heap-worker n)
  (if (= n 0)
      'done
      (begin
        (cons 1 (cons 2 (cons 3 'nil)))
        (heap-worker (- n 1)))))

(define (worker waiter-pid)
  (heap-worker 500)
  (send waiter-pid 'done))

(define (spawn-loop n waiter-pid)
  (if (= n 0)
      'done
      (begin
        (spawn (lambda () (worker waiter-pid)))
        (spawn-loop (- n 1) waiter-pid))))

(define (waiter count)
  (if (= count 0)
      (print "all done")
      (begin
        (recv)
        (waiter (- count 1)))))

(define (main)
  (spawn-loop 50 (self))
  (waiter 50))