;; Phase 1 test: spawn many actors
;; Expected: completes without crash

(define (idle)
  (recv))

(define (spawner n)
  (if (= n 0)
      'done
      (begin
        (spawn 'idle)
        (spawner (- n 1)))))

(define (main)
  (spawner 1000)
  (print "1000 actors spawned"))