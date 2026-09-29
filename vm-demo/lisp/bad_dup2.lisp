; 负例（与 bad_dup1 链接）：跨单元重复 def 同名 —— 链接期 duplicate global
(def dup (lambda () 6))
(dup)