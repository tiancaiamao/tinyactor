((define (main) (let r (vm.nosuchfn 1 2) (let again (vm.nosuchfn 3 4) (if (and (null? r) (null? again)) (print "ok") (print "FAIL"))))))
