;;; ============================================================================
;;; interp.scm — golden interpreter core for the TinyActor kernel fuzzing
;;;              toolchain (docs/kernel-fuzzing-design.md §5.3, DELIV-2)
;;;
;;; MODULE BOUNDARY (task-interp-core):
;;;   This file currently implements only the foundation:
;;;     * value model (Guile representation of TA values)
;;;     * int48 normalization (w48)
;;;     * arithmetic / comparison primitives (+ - * / % and the full
;;;       comparison set, with TA short-circuit && ||)
;;;     * print_val stringification for int/bool/nil/string/symbol
;;;       (pair/list branch is stubbed and reserved)
;;;
;;; LATER TASKS (task-interp-full) add on top of this file:
;;;     * closures / environments + application
;;;     * match / let / if evaluation over the dumped AST
;;;     * print_val pair/list branches (dotted pair support)
;;;
;;; Semantics source of truth (all cross-checked against tavm/src/vm.c and
;;; docs/kernfuzz-facts.md):
;;;   * Int is int48, range [-2^47, 2^47). Arithmetic wraps silently (src/val.c).
;;;   * Integer / and % truncate toward zero with remainder sign following the
;;;     dividend (C int64 semantics — VM uses C / and %). Scheme quotient /
;;;     remainder match this exactly. Modulo is FORBIDDEN for / and % (it is
;;;     used only inside w48, where the modulus 2^48 is non-negative).
;;;   * Division/modulo by zero kills the process with reason 'divzero
;;;     (src/vm.c OP_DIV/OP_MOD) → we raise the (divzero) condition; the
;;;     upper layer (main.scm) catches it and emits the DIVZERO protocol line.
;;;   * == on strings is content equality; on everything else it is identity
;;;     (src/vm.c val_equal) — pairs compare by pointer identity, so two
;;;     structurally equal but separately constructed lists are NOT equal
;;;     (kernfuzz-facts f1).
;;;   * < <= > >= are numeric-only: with any non-int operand they return false
;;;     (src/vm.c OP_LT/OP_LE — non-ints never satisfy the comparison).
;;;   * && || are binary short-circuit operators (docs/ta-language-spec.md
;;;     《运算符》).
;;;   * print_val: string is printed raw — no quotes, no escape processing
;;;     (src/vm.c print_val).
;;;
;;; Design constraint (§5.3): ALL integers are Scheme arbitrary-precision;
;;; w48 normalization happens ONLY after each arithmetic operation. Never use
;;; fixnum/int64 intermediate values (that would double-truncate).
;;;
;;; Usage: this is a library. Tests load it and run assertions:
;;;   guile --no-auto-compile -s tools/kernfuzz/golden/test-interp-core.scm
;;; ============================================================================

(use-modules (srfi srfi-35))            ; define-condition-type / make-condition

;;; --- value model ------------------------------------------------------------

;; TA nil is a DISTINCT value (a unique uninterned symbol). Using an
;; uninterned symbol (make-symbol) — not a symbol literal — guarantees it can
;; never collide with a TA symbol read from an AST dump (dump symbols are
;; interned by string->symbol). 'nil must be tested with (eq? v *nil*) only.
(define *nil* (make-symbol "nil"))

;; bool  -> #t / #f
;; int   -> arbitrary-precision integer (always w48-normalized after ops)
;; string-> Guile string (raw bytes, no escape processing)
;; symbol-> Guile symbol
;; pair  -> Guile pair (proper lists are chains ending in *nil*; dotted
;;          pairs end in any non-*nil* value — natural in Scheme)
;; closure-> (closure params body env) record — added by task-interp-full

;; predicate helpers used by the arithmetic/comparison layer
(define (ta-nil? v) (eq? v *nil*))
(define (ta-int? v) (exact-integer? v))
(define (ta-bool? v) (or (eq? v #t) (eq? v #f)))

;;; --- int48 normalization ------------------------------------------------------

(define *2^48* (expt 2 48))
(define *2^47* (expt 2 47))

;; Normalize n into the int48 domain [-2^47, 2^47).
;; n may be any arbitrary-precision integer. Mirrors the VM: val_int() keeps
;; the low 48 bits of the payload and val_get_int() sign-extends.
(define (w48 n)
  (let ((m (modulo n *2^48*)))
    (if (>= m *2^47*)
        (- m *2^48*)
        m)))

;;; --- arithmetic / comparison primitives ---------------------------------------

;; (divzero) condition: raised on integer division / modulo by zero.
;; The upper layer (main.scm) catches it and prints the DIVZERO protocol line
;; (§5.3); within this module it is simply a non-value object.
(define-condition-type divzero-error &condition
  divzero-error?)

;; every arithmetic result is w48-normalized (never a fixnum/int64 shortcut)
(define (ta-add a b) (w48 (+ a b)))
(define (ta-sub a b) (w48 (- a b)))
(define (ta-mul a b) (w48 (* a b)))

;; integer division, truncating toward zero; quotient only (see module header)
(define (ta-div a b)
  (if (= b 0)
      (raise-exception (make-condition divzero-error))
      (w48 (quotient a b))))

;; integer remainder, sign follows the dividend (C %); remainder only
(define (ta-mod a b)
  (if (= b 0)
      (raise-exception (make-condition divzero-error))
      (w48 (remainder a b))))

;; numeric comparisons: false whenever either operand is not an int
;; (src/vm.c OP_LT/OP_LE — non-int operands never satisfy the comparison;
;;  OP_GT/OP_GE are the mirror images of the same codegen)
(define (ta-lt a b)
  (and (ta-int? a) (ta-int? b) (< a b)))
(define (ta-le a b)
  (and (ta-int? a) (ta-int? b) (<= a b)))
(define (ta-gt a b)
  (and (ta-int? a) (ta-int? b) (> a b)))
(define (ta-ge a b)
  (and (ta-int? a) (ta-int? b) (>= a b)))

;; == : strings compare by content (val_equal); ints/bools/nil/symbols compare
;; by value; EVERYTHING else (pair/list/closure/pid) compares by identity —
;; two separately constructed equal-looking pairs are NOT equal (f1).
(define (ta-eq a b)
  (cond
   ((and (string? a) (string? b)) (string=? a b))
   ((and (ta-int? a) (ta-int? b)) (= a b))
   (else (eq? a b))))

;; logical and/or: TA semantics = binary, short-circuit (spec《运算符》)
(define (ta-and a b) (and a b))
(define (ta-or a b) (or a b))

;;; --- print_val core (int/bool/nil/string/symbol) --------------------------------

;; Stringify v exactly as the VM's print_val does (src/vm.c), WITHOUT the
;; trailing newline (that is added by print, not by print_val).
;; Current scope: int/bool/nil/string/symbol. The pair branch — recursive
;; along the cdr chain, " . " for dotted tails — is implemented by
;; print-pair (reserved for task-interp-full) and wired in below.
(define (print-pair v)
  ;; placeholder: full cdr-chain rendering lands with task-interp-full
  (error "print-pair: pair printing not yet implemented (task-interp-full)"))

(define (print_val v)
  (cond
   ((ta-int? v) (number->string v))           ; decimal with sign; v is already w48-normalized
   ((ta-nil? v) "nil")
   ((eq? v #t) "true")
   ((eq? v #f) "false")
   ((symbol? v) (symbol->string v))           ; name, verbatim
   ((string? v) v)                            ; raw bytes: no quotes, no escapes
   ((pair? v) (print-pair v))                 ; reserved for task-interp-full
   (else "?")))                               ; catch-all, mirrors the VM

;; print with the VM's trailing newline (each print flushes; the flush is a
;; runner concern, not this module's)
(define (ta-print v)
  (display (print_val v))
  (newline))