;; Test: actor-selective-recv
;; Purpose: Actor with selective receive — skips messages that don't
;;          match, leaves them for later. Tests mailbox behavior when
;;          messages are consumed non-FIFO (selective recv pattern).
;;          This is an advanced Phase 2 pattern.
;; Expected output: got important\nalso got 99
;; Note: EXPECTED-FAIL — selective receive not yet implemented

(define (processor)
  (match (recv)
    (('important v)
     (print "got important")
     (processor))
    (('info v)
     (if (= v 99)
         (print "also got 99")
         (processor)))
    ('stop 'done)))

(define (main)
  (let pid (spawn 'processor))
  (send pid (cons 'info 1))
  (send pid (cons 'info 2))
  (send pid (cons 'important 42))
  (send pid (cons 'info 99))
  (send pid 'stop)
  (let ref (monitor pid))
  (recv))