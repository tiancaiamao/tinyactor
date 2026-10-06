((define (f lst) (match lst (nil 0) ((cons a (cons b rest)) (+ 1 (f (cons b rest)))) ((cons a rest) (+ 1 (f rest))))) (define (main) (print (f (cons 1 (cons 2 (cons 3 nil)))))))
