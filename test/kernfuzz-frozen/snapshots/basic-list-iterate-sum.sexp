((define (sum_list lst acc) (if (null? lst) acc (sum_list (cdr lst) (+ acc (car lst))))) (define (build n) (if (= n 0) nil (cons n (build (- n 1))))) (define (main) (print (sum_list (build 100) 0))))
