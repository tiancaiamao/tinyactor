;; Test: recv-scan — selective receive
;; Purpose: (receive ...) scans the mailbox and SKIPS messages whose
;;          patterns don't match, leaving them for a later receive.
;;          This proves non-FIFO behavior impossible with plain
;;          (recv)+(match) (which is strictly FIFO).
;;
;; Mailbox arrives in order: 'first, 'second, 42
;;   receive 1 takes 'second FIRST  (skips 'first)
;;   receive 2 takes 'first         (it survived in the mailbox)
;;   receive 3 takes 42             (variable catch-all)
;;
;; Expected output (in order):
;;   got-second
;;   then-first
;;   PASS
;;   server-done

(define (server)
  (receive
    ('second (print "got-second")))
  (receive
    ('first (print "then-first")))
  (receive
    (n (if (= n 42) (print "PASS") (print "FAIL-INT")))
    ('junk (print "FAIL-ANY"))))

(define (main)
  (let srv (spawn (lambda () (server))))
  (send srv 'first)
  (send srv 'second)
  (send srv 42)
  (let ref (monitor srv))
  (receive
    (('DOWN r pid reason) (print "server-done"))))