((import "str") (define (main) (let s "hello" (begin (print (str.length s)) (print (str.concat s " world")) (print (str.chr 65)) (print (str.concat (str.chr 66) (str.chr 67)))))))
