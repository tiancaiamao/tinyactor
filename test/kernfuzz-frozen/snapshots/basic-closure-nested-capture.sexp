((define (f x) (lambda (y) (lambda (z) (+ x (+ y z)) nil) nil)) (define (main) (let g (f 1) (let h (g 2) (print (h 3))))))
