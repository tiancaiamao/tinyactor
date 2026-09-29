; prelude.lisp — 每个 lisp 程序前置的引导形式。
;
; extern 是**编译期**声明：编译器不知道 C 侧的 native 表（否则要维护两份），
; 只把名单写进 .bc 尾部的 extern 段，由 link_unit 按名绑定。写错名字照样在
; 链接期报 undefined global——typo 不会静默变成运行期崩。
;
; null?/not 原来住在这里，剥到最小内核时连同多单元链接器一起删了，于是它们
; 变成未定义名。改成「把本文件的 forms 直接前置到用户程序」：不需要链接器，
; prelude 就是普通的顶层 extern/def。
;
; 为什么是 .lisp 文件而不是在 main.ta 里用 cons 拼：lisp 形式是**异构**数据
; （symbol / int / bool / null 混在一条链上），而 TA 的类型系统会在 unify 点
; 拒绝异构 cons——在 TA 里拼这个 prelude 要么加 any，要么一层层套参数把
; 字面量藏进松类型里，都比直接读个文件贵。写成 .lisp 走 sexp.parse 就不碰
; 类型层：这正是它该在的层，prelude 是 lisp 源码，不是 TA 代码。
(extern print println)

(def null? (lambda (x) (eq? x nil)))

(def not (lambda (x) (if x false true)))