; 纯 TCALL 循环基准（解释器吞吐）；(loop 1 1000 0) => 59542
(def coll (lambda (n acc) (if (eq? n 1) acc
  (coll (if (eq? (* (/ n 2) 2) n) (/ n 2) (+ (* 3 n) 1)) (+ acc 1)))))
(def loop (lambda (i stop acc) (if (> i stop) acc
  (loop (+ i 1) stop (+ acc (coll i 0))))))
(loop 1 1000 0)