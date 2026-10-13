; 尾调用 + 列表原语；链表用 cons 显式构造（quote 仅生成符号）
; (sum (cons 1 ... (cons 5 nil))) 0 => 15
; null? 本地定义（单编译单元；库/链接机制继承 TA，tavm 不重做）
(def sum (lambda (xs acc) (if (null? xs) acc (sum (cdr xs) (+ acc (car xs))))))
(sum (cons 1 (cons 2 (cons 3 (cons 4 (cons 5 nil))))) 0)