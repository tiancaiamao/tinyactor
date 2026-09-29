; 负例：def 体捕获未声明名 g —— 编译期拦截（2.2），
; 旧行为是静默捕获进 fvs，运行期 LOADF on non-closure
(def f (lambda (x) (g x)))
1