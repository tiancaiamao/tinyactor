;; Test: error-send-to-dead
;; Purpose: Sending a message to a dead process should silently discard
;;          the message, not crash the sender. Also tests monitoring
;;          an already-dead process.
;; Expected output: sender alive

(define (quick-die)
  (/ 1 0))

(define (main)
  (let dead-pid (spawn 'quick-die))
  (let ref (monitor dead-pid))
  (recv)
  (send dead-pid 'hello)
  (print "sender alive"))