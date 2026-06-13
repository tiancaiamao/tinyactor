;; Test: pattern-match-pair-destructure
;; Purpose: Pattern matching on pairs to destructure messages.
;;          Tests MATCH_PAIR opcode with various patterns.
;; Expected output: got 1 and 2\ngot symbol hello

(define (test-pair)
  (let p (cons 1 2))
  (match p
    ((cons a b) (print "got 1 and 2"))))

(define (test-symbol)
  (match 'hello
    ('hello (print "got symbol hello"))
    (_ (print "no match"))))

(test-pair)
(test-symbol)