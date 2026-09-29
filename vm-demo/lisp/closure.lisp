; 闭包捕获 + CLOS_ID 直调 + eq?/quote
; (+ (+ ((add 5) 37) (forty)) (if (eq? 'a 'a) 1 0)) => 85
; v1 不支持右值非 lambda 的值定义——((add 5) 37) 内联部分应用表达同语义
(def add (lambda (a) (lambda (b) (+ a b))))
(def forty (lambda () 42))
(+ (+ ((add 5) 37) (forty)) (if (eq? 'a 'a) 1 0))