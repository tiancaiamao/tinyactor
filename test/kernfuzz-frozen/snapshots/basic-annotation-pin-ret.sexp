((define (id x) x) (type-sig h (nil) Int) (define (h x) (id x)) (define (main) (print (str.from_int (h 42)))))
