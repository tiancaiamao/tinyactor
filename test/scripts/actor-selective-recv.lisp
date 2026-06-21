;; Test: actor-selective-recv
;; Purpose: Actor with selective receive — skips messages that don't
;;          match, leaves them for later. Tests mailbox behavior when
;;          messages are consumed non-FIFO (selective recv pattern).
;;          This is an advanced Phase 2 pattern.
;; Expected output: got important\nalso got 99

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
  (send pid (cons 'info (cons 1 '())))
  (send pid (cons 'info (cons 2 '())))
  (send pid (cons 'important (cons 42 '())))
  (send pid (cons 'info (cons 99 '())))
  (send pid 'stop)
  (let ref (monitor pid))
  (recv))