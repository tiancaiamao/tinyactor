; 1M 基准（同 collatz.lisp 负载：1..999999 steps 从 1 起取 max）=> 525
(def coll (lambda (n acc) (if (eq? n 1) acc
  (coll (if (eq? (* (/ n 2) 2) n) (/ n 2) (+ (* 3 n) 1)) (+ acc 1)))))
(def mx (lambda (a b) (if (> a b) a b)))
(def upto (lambda (i limit best) (if (>= i limit) best
  (upto (+ i 1) limit (mx (coll i 1) best)))))
(upto 1 1000000 0)