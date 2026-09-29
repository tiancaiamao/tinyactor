; cfunc 作一等函数值：extern 名编译成 GLOBAL（CLOS_ID），能传参、能被 CALL 调用
; (map print (1 2)) => 打印 1 / 2，返回 (1 2)
(extern print)
(extern null?)
(def map (lambda (f xs) (if (null? xs) nil (cons (f (car xs)) (map f (cdr xs))))))
(map print (cons 1 (cons 2 nil)))