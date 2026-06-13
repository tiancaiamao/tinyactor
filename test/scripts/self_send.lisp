;; Phase 1 test: self-send-recv
;; Expected output: 42

(define (main)
  (send (self) 42)
  (print (recv)))