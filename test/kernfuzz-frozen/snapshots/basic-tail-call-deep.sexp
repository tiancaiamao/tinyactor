((define (sum r i) (if (= i 0) r (sum (+ r 1) (- i 1)))) (define (main) (print (sum 0 5000000))))
