((define (main) (let r (vm.nosuchfn 1 2) (if (null? r) (print "ok") (print "FAIL")))))
