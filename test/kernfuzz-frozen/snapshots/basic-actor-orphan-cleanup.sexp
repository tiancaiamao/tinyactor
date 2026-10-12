((define (main) (begin (spawn (lambda nil (let _ (recv) nil) nil)) (let _ (recv_after 100)) (print "PASS"))))
