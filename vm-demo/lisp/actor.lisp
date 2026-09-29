; actor 纵切（4.3）：spawn / send / recv / 阻塞让出 / 跨进程消息
; 期望输出：2（spawn 返回新 pid 2）、7（子 proc 算出 6+1 回传）、7（末表达式值）
;
; 两段：
;   1. 父 spawn 一个闭包子 proc，不存 pid——spawn 的返回值直接进 send 实参位
;   2. 父 proc 阻塞在 recv（邮箱空 → 让出调度器），子 proc 跑完发回后被唤醒
;
; 闭包自由变量跨进程捕获（子 proc 用捕获到的父 pid 回发）暂不在此覆盖：
; 那需要父帧的 let 局部，而 compile.ta 的 let 槽与求值栈共用 base+1..，
; let 存的 pid 会被随后的 PUSH 覆盖。详见 README「已知缺陷」。
(extern spawn)
(extern send)
(extern recv)
(extern print)

; 子 proc：recv 拿到 6，+1 后 send 回父 proc（父是 pid 1）
(def child
  (lambda ()
    (send 1 (+ 1 (recv)))))

(send (spawn child) 6)

(print (recv))