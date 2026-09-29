; 尾调用 + 列表原语；链表用 cons 显式构造（quote 仅生成符号）
; (sum (cons 1 ... (cons 5 nil))) 0 => 15
; null? 来自库（prelude.bc，可从 eq?/nil 推导，不烧 opcode）
(extern null?)
(def sum (lambda (xs acc) (if (null? xs) acc (sum (cdr xs) (+ acc (car xs))))))
(sum (cons 1 (cons 2 (cons 3 (cons 4 (cons 5 nil))))) 0)