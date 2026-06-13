;; Test: string-in-list
;; Purpose: Strings stored in lists, sent between processes.
;;          Verifies string deep copy works correctly when strings
;;          are nested inside pairs.
;; Expected output: hello world

(define (worker)
  (match (recv)
    ((cons 'data lst)
     (print (string-concat (car lst) (car (cdr lst)))))))

(define (main)
  (let msg (cons "hello " (cons "world" 'nil)))
  (let pid (spawn (lambda () (worker))))
  (send pid (cons 'data msg))
  (let ref (monitor pid))
  (recv))