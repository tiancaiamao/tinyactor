((define (add a b) (+ a b)) (define (curry f x) (lambda (y) (f x y) nil)) (define (main) (let g (curry add 3) (print (g 2)))))
