;; Test: multithread-msg
;; Purpose: Verify message CONTENT survives the heap-fragment
;;          send/recv roundtrip (frag_copy → val_deep_copy).
;;          Exercises symbol/string/int crossing process heaps.
;; Expected output: PASS

(define (receiver)
  (match (recv)
    (('hello . rest)
     ;; rest = ("world" . 42)
     (if (string-eq (car rest) "world")
         (if (= (cdr rest) 42)
             (print "PASS")
             (print "FAIL: int"))
         (print "FAIL: string")))
    (_ (print "FAIL: pattern"))))

(define (main)
  (let b (spawn (lambda () (receiver))))
  (send b (cons 'hello (cons "world" 42))))