((define (adder n) (lambda (x) (+ x n) nil)) (define (main) (let g (adder 5) (print (g 3)))))
