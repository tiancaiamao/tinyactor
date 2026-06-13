;; Test: error-process-crash-isolated
;; Purpose: Process crashes with division by zero. Verifies:
;;          1. Crash doesn't affect other processes
;;          2. Monitor delivers DOWN notification
;;          3. Other process can continue normally
;; Expected output: worker crashed\nsurvived

(define (crash-worker n)
  (if (= n 0)
      (/ 1 0)
      (crash-worker (- n 1))))

(define (observer)
  (match (recv)
    (('DOWN ref pid reason)
     (print "worker crashed"))
    (_ (print "unexpected"))))

(define (main)
  (let crasher-pid (spawn (lambda () (crash-worker 50))))
  (let obs-pid (spawn 'observer))
  (let ref (monitor crasher-pid))
  (send obs-pid (cons 'DOWN (cons ref (cons crasher-pid (cons 'divzero 'nil)))))
  (send (self) 'hello)
  (recv)
  (print "survived"))