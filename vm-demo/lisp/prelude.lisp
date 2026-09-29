; prelude.lisp — lisp 层库：可从最小自举集推导的原生函数。
; opcode 只留不可推导集（算术/比较/cons/car/cdr/eq?/pair?/symbol?），
; null?/not 这类能写成一行的推导函数住这里（2.2 库机制）。
; 链接：lispvm prog.bc prelude.bc —— prog 侧 (extern ...) 声明，loader 补项。
(def null? (lambda (x) (eq? x nil)))
(def not (lambda (x) (if x false true)))