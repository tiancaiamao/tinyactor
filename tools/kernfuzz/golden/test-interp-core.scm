;;; ============================================================================
;;; test-interp-core.scm — unit tests for tools/kernfuzz/golden/interp.scm
;;;
;;; Covers the task-interp-core scope: value model, w48 normalization,
;;; arithmetic/comparison primitives (incl. / % truncation and divzero
;;; condition), and the print_val core (int/bool/nil/string/symbol).
;;;
;;; Run standalone (exit 0 = all pass, exit 1 + message = failure):
;;;   guile --no-auto-compile -s tools/kernfuzz/golden/test-interp-core.scm
;;; ============================================================================

(use-modules (ice-9 format))

;; Resolve interp.scm relative to this file (guile -s chdir's to the script's
;; directory and resolves relative loads against it, but this belt-and-braces
;; approach also works when invoked from an arbitrary cwd).
(load (if (current-filename)
          (let ((d (dirname (current-filename))))
            (string-append d "/interp.scm"))
          "tools/kernfuzz/golden/interp.scm"))

;; --- tiny assertion harness ---------------------------------------------------

(define *failures* 0)
(define *checks* 0)

(define (fail! label expected got)
  (set! *failures* (+ *failures* 1))
  (format (current-error-port)
          "FAIL: ~a~%      expected: ~s~%      got:      ~s~%"
          label expected got))

(define (assert-eq label expected got)
  (set! *checks* (+ *checks* 1))
  (if (equal? expected got)
      #t
      (fail! label expected got)))

(define (assert-true label got)
  (set! *checks* (+ *checks* 1))
  (if got #t (fail! label #t got)))

(define (assert-error label pred thunk)
  (set! *checks* (+ *checks* 1))
  (catch #t
    (lambda ()
      (thunk)
      (fail! label "<error raised>" "no error"))
    (lambda (key . args)
      ;; raised condition lands as the first arg (ice-9 exceptions catch
      ;; protocol: key=%exception, args=(<condition>))
      (let ((cond (if (pair? args) (car args) key)))
        (if (pred cond)
            #t
            (fail! label (list "<condition matching" pred ">")
                   (if (pair? args) (car args) (cons key args))))))))

;; --- value model ----------------------------------------------------------------

(assert-true "nil is a distinct value (not any interned symbol)"
             (and (ta-nil? *nil*)
                  (not (eq? *nil* 'nil))
                  (symbol? *nil*)))
(assert-true "nil does not collide with an interned symbol read from a dump"
             (not (eq? *nil* (string->symbol "nil"))))

;; --- w48 / wrap-around ------------------------------------------------------------

;; DELIV-2 scenario 2: 140737488355327 (= 2^47 - 1) + 1 wraps to -2^47
(assert-eq "wrap: 140737488355327 + 1 -> -140737488355328"
           -140737488355328
           (ta-add 140737488355327 1))

;; negative wrap: (0 - 2^47) - 1 wraps to 2^47 - 1
(assert-eq "wrap: (0 - 2^47) - 1 -> 140737488355327"
           140737488355327
           (ta-sub (- *2^47*) 1))

;; boundary stability: 2^47 - 1 stays put, -2^47 stays put
(assert-eq "boundary: w48 140737488355327 stays"
           140737488355327
           (w48 140737488355327))
(assert-eq "boundary: w48 -140737488355328 stays"
           -140737488355328
           (w48 -140737488355328))
(assert-eq "boundary: w48 2^47 wraps to -2^47"
           -140737488355328
           (w48 *2^47*))

;; --- arithmetic primitives --------------------------------------------------------

(assert-eq "+ wraps inside the int48 domain"
           6
           (ta-add 3 3))
(assert-eq "multiplication wrap: (2^23+1)*(2^23+1) == hand-computed int48 mod"
           (let ((x (+ (expt 2 23) 1))
                 (raw (* (+ (expt 2 23) 1) (+ (expt 2 23) 1))))
             (w48 raw))
           (ta-mul (+ (expt 2 23) 1) (+ (expt 2 23) 1)))
;; (2^23+1)^2 = 2^46 + 2^24 + 1 < 2^47, so this is also a no-wrap control
(assert-eq "multiplication no-wrap control: (2^23+1)^2 exact"
           (+ (expt 2 46) (expt 2 24) 1)
           (ta-mul (+ (expt 2 23) 1) (+ (expt 2 23) 1)))
;; a genuinely wrapping product: 2^47 * 2^47 = 2^94 ≡ 0 (mod 2^48)
(assert-eq "multiplication wrap: 2^47 * 2^47 -> 0"
           0
           (ta-mul *2^47* *2^47*))

;; --- division / remainder: truncation toward zero ---------------------------------

;; DELIV-2 scenario 3: 7 / (0-2) truncates toward zero -> -3
(assert-eq "trunc-div: 7 / (0 - 2) -> -3"
           -3
           (ta-div 7 (- 0 2)))
(assert-eq "trunc-div: (0 - 7) / 2 -> -3"
           -3
           (ta-div (- 0 7) 2))
(assert-eq "trunc-div: 7 / 2 -> 3"
           3
           (ta-div 7 2))

;; TA remainder sign follows the dividend (C %), matching Scheme remainder
(assert-eq "mod: 7 % (0 - 2) -> 1"
           1
           (ta-mod 7 (- 0 2)))
(assert-eq "mod: (0 - 7) % 2 -> -1"
           -1
           (ta-mod (- 0 7) 2))
(assert-eq "mod: (0 - 7) % (0 - 2) -> -1"
           -1
           (ta-mod (- 0 7) (- 0 2)))
(assert-eq "mod: 7 % 2 -> 1"
           1
           (ta-mod 7 2))

;; --- divzero condition ---------------------------------------------------------------

(assert-error "1 / 0 raises (divzero) condition" divzero-error?
              (lambda () (ta-div 1 0)))
(assert-error "0 / 0 raises (divzero) condition" divzero-error?
              (lambda () (ta-div 0 0)))
(assert-error "1 % 0 raises (divzero) condition" divzero-error?
              (lambda () (ta-mod 1 0)))
(assert-true "valid division does not raise"
             (catch #t
               (lambda () (ta-div 6 3) #t)
               (lambda (k . a) #f)))

;; --- comparisons ---------------------------------------------------------------------

(assert-eq "== 5 5 -> true" #t (ta-eq 5 5))
(assert-eq "== 5 6 -> false" #f (ta-eq 5 6))
(assert-eq "== \"ab\" \"ab\" -> true (content)" #t (ta-eq "ab" "ab"))
(assert-eq "== \"ab\" \"ac\" -> false" #f (ta-eq "ab" "ac"))
(assert-eq "== true true -> true" #t (ta-eq #t #t))
(assert-eq "== nil nil -> true" #t (ta-eq *nil* *nil*))
(assert-true "== pair identity: same object is equal"
             (let ((p (cons 1 2)))
               (ta-eq p p)))
(assert-eq "== pair identity: structurally equal, separately built -> false (f1)"
           #f
           (ta-eq (cons 1 2) (cons 1 2)))
(assert-true "== pair identity: aliased bindings are equal (f1: c==a)"
             (let ((a (cons 1 2)))
               (let ((c a))
                 (ta-eq c a))))
(assert-eq "== int vs bool -> false" #f (ta-eq 1 #t))

(assert-true "< 1 2" (ta-lt 1 2))
(assert-eq "< 2 1 -> false" #f (ta-lt 2 1))
(assert-true "<= 2 2" (ta-le 2 2))
(assert-true "> 3 2" (ta-gt 3 2))
(assert-true ">= 3 3" (ta-ge 3 3))
;; comparisons on non-int operands are false (src/vm.c OP_LT/OP_LE)
(assert-eq "< true 2 -> false" #f (ta-lt #t 2))
(assert-eq "< 1 \"x\" -> false" #f (ta-lt 1 "x"))
(assert-eq "<= nil nil -> false" #f (ta-le *nil* *nil*))

;; --- short-circuit && || ----------------------------------------------------------------
;;
;; At this layer (task-interp-core) ta-and/ta-or are value-level helpers on
;; booleans; the actual SHORT-CIRCUIT property is an evaluator concern — the
;; interpreter (task-interp-full) must evaluate the left operand first and
;; skip the right one when it decides the result (('and a b) / ('or a b) AST
;; nodes). Truth tables are pinned here; short-circuit itself is verified in
;; task-interp-full's evaluator tests.
(assert-eq "true && true -> true" #t (ta-and #t #t))
(assert-eq "true && false -> false" #f (ta-and #t #f))
(assert-eq "false && true -> false" #f (ta-and #f #t))
(assert-eq "false && false -> false" #f (ta-and #f #f))
(assert-eq "false || false -> false" #f (ta-or #f #f))
(assert-eq "false || true -> true" #t (ta-or #f #t))
(assert-eq "true || false -> true" #t (ta-or #t #f))
(assert-eq "true || true -> true" #t (ta-or #t #t))

;; --- print_val core ---------------------------------------------------------------------

(assert-eq "print int (w48-normalized -5) -> \"-5\""
           "-5"
           (print_val (w48 -5)))
(assert-eq "print int 0 -> \"0\"" "0" (print_val 0))
(assert-eq "print int 42 -> \"42\"" "42" (print_val 42))
(assert-eq "print int 2^47-1 -> decimal"
           "140737488355327"
           (print_val (- *2^47* 1)))
(assert-eq "print int -2^47 -> decimal with sign"
           "-140737488355328"
           (print_val (- *2^47*)))
(assert-eq "print nil -> \"nil\"" "nil" (print_val *nil*))
(assert-eq "print true -> \"true\"" "true" (print_val #t))
(assert-eq "print false -> \"false\"" "false" (print_val #f))
(assert-eq "print symbol -> name verbatim"
           "hello"
           (print_val (string->symbol "hello")))
;; string is printed raw: no quotes, no escape processing (src/vm.c print_val)
(assert-eq "print string \"abc\" -> raw bytes, no quotes"
           "abc"
           (print_val "abc"))
(assert-eq "print string with embedded newline -> raw bytes kept"
           "a\nb"
           (print_val "a\nb"))
(assert-eq "print string with backslash -> raw backslash kept"
           "a\\b"
           (print_val "a\\b"))

;; --- summary -----------------------------------------------------------------------------

(display (format #f "~a checks, ~a failures~%" *checks* *failures*))
(if (zero? *failures*)
    (begin (display "ALL PASS\n") (exit 0))
    (begin (format (current-error-port) "~a FAILURES~%" *failures*)
           (exit 1)))