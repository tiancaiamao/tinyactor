((define (make_closure x) (lambda (y) (lambda (z) (+ x (+ y z)) nil) nil)) (define (main) (let f1 (make_closure 20) (let f2 (f1 10) (print (f2 12))))))
