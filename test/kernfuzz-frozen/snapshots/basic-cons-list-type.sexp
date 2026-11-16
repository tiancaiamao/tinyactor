((import "tinyactor:lib/list" "tinyactor:lib/list") (type-sig sum_len ((List Int)) Int) (define (sum_len xs) (list.length xs)) (define (main) (print (sum_len (cons 1 (cons 2 nil))))))
