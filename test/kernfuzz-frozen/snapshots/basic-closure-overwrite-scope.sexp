((define (test) (let x 1 (let f (lambda nil x nil) (let x 99 (f))))) (define (main) (print (test))))
