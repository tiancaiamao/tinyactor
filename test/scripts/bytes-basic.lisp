;; Test: bytes-basic
;; Purpose: Basic bytes type operations — creation and length.
;;          Phase 2 adds bytes type support alongside strings.
;; Expected output: 4
;; Note: EXPECTED-FAIL — bytes type not yet implemented

(define (main)
  (let b (bytes 1 2 3 4))
  (print (bytes-length b)))