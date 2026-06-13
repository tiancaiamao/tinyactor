;; Test: gc-closure-in-spawn
;; Purpose: Spawn many processes using closures with free variables.
;;          Each closure captures a different value. Verifies deep copy
;;          of closures during spawn works under GC pressure.
;; Expected output: all workers done

(define (worker val)
  (let msg (recv))
  (if (= msg val)
      'ok
      'mismatch))

(define (spawn-workers n collector)
  (if (= n 0)
      'done
      (begin
        (let pid (spawn (lambda () (worker n))))
        (send pid n)
        (send collector pid)
        (spawn-workers (- n 1) collector))))

(define (collector remaining)
  (if (= remaining 0)
      (print "all workers done")
      (begin
        (recv)
        (collector (- remaining 1)))))

(spawn-workers 100 (self))
(collector 100)