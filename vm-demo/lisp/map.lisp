; lisp1 验证：匿名 lambda 作函数值传参，参数名统一调用（Erlang/Gleam 风格）
; (map (lambda (x) (* x x)) ...) => (1 4 9)
(def null? (lambda (x) (eq? x nil)))
(def map (lambda (f xs) (if (null? xs) nil (cons (f (car xs)) (map f (cdr xs))))))
(map (lambda (x) (* x x)) (cons 1 (cons 2 (cons 3 nil))))