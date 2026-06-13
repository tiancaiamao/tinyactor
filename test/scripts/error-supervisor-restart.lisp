;; Test: error-supervisor-restart
;; Purpose: Supervisor monitors a worker that crashes and restarts it.
;;          Tests the full crash → DOWN → restart cycle.
;; Expected output: worker died\nworker died\nworker died\ngiving up

(define (worker id)
  (let msg (recv))
  (if (= msg 'crash)
      (/ 1 0)
      (begin
        (print msg)
        (worker id))))

(define (supervisor)
  (let pid (spawn (lambda () (worker 0))))
  (let ref (monitor pid))
  (send pid 'crash)
  (sup-loop pid ref 0))

(define (sup-loop pid ref count)
  (match (recv)
    (('DOWN r p reason)
     (print "worker died")
     (if (< count 2)
         (begin
           (let new-pid (spawn (lambda () (worker (+ count 1)))))
           (let new-ref (monitor new-pid))
           (send new-pid 'crash)
           (sup-loop new-pid new-ref (+ count 1)))
         (print "giving up")))))

(define (main)
  (let sup-pid (spawn 'supervisor))
  (let ref (monitor sup-pid))
  (recv))