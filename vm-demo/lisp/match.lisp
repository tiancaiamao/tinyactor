; match desugar（4.2）：模式 → 谓词 + if 树，VM 零改动
; 覆盖：字面量 / 变量兜底 / _ 通配 / pair 模式 / 无匹配 → nil
(def lst (lambda (a b c d e) (cons a (cons b (cons c (cons d (cons e nil)))))))
(lst (match 2 (1 10) (2 20) (3 30))            ; 字面量命中 => 20
    (match 7 (1 10) (x (* x 2)))                ; 变量模式兜底 => 14
    (match 7 (1 10) (_ 99))                     ; _ 通配 => 99
        (match 5 (nil 0) ((a b) 1))                 ; 无匹配 => nil
    (match (cons 1 (cons 2 nil))                ; 嵌套 pair 模式 => 3
      ((a (b c)) (+ a b))))