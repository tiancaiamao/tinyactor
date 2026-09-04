((define (count n) (if (= n 0) n (count (- n 1)))) (define (main) (print (count 1000000))))
