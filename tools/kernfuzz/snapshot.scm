;;; snapshot.scm — kernfuzz corpus AST snapshot generator
;;;
;;; Scans test/basic/*.ta + test/compiler/*.ta, skips negative-case files
;;; (names containing -errors / -parse-errors), and dumps each legal file's
;;; full AST via:   ./tinyactor run tools/kernfuzz/ast-dump.ta <file>
;;;
;;; Success (exit 0 + non-empty stdout) ->
;;;   test/kernfuzz-frozen/snapshots/<dir>-<name>.sexp   (flat naming:
;;;   test/basic/closure.ta -> snapshots/basic-closure.sexp)
;;; Failure (non-zero exit or empty stdout) -> one line in
;;;   test/kernfuzz-frozen/expected-fail.txt
;;;   format: <file>\t<exit_code>\t<stderr first line, clipped>
;;;
;;; Determinism: files processed in sorted order; all stale *.sexp are
;;; removed before regenerating; repeated runs produce byte-identical sets.
;;;
;;; Usage:  guile tools/kernfuzz/snapshot.scm      (run from repo root)

(use-modules (ice-9 popen)
             (ice-9 ftw)
             (srfi srfi-1)
             (srfi srfi-13))

(define *snap-dir* "test/kernfuzz-frozen/snapshots")
(define *fail-path* "test/kernfuzz-frozen/expected-fail.txt")
(define *sources* '("test/basic" "test/compiler"))
(define *out-tmp* "/tmp/kernfuzz-snap-stdout.tmp")
(define *err-tmp* "/tmp/kernfuzz-snap-stderr.tmp")

;; --- small file/string helpers ----------------------------------------------

;; read one line (no trailing newline); guile 3.0 has no built-in read-line
(define (read-line port)
  (let loop ((acc '()))
    (let ((c (read-char port)))
      (if (or (eof-object? c) (char=? c #\newline))
          (apply string-append (reverse acc))
          (loop (cons (string c) acc))))))

(define (read-file-string path)
  (call-with-input-file path
    (lambda (port)
      (let loop ((acc '()))
        (let ((c (read-char port)))
          (if (eof-object? c)
              (apply string-append (reverse acc))
              (loop (cons (string c) acc))))))))

(define (write-file-string path content)
  (let ((port (open-output-file path)))  ; opens with truncate by default
    (display content port)
    (close-output-port port)))

(define (first-line s)
  (let ((i (string-index s #\newline)))
    (if i (string-take s i) s)))

(define (clip s n)
  (if (> (string-length s) n) (string-take s n) s))

;; --- corpus rules -------------------------------------------------------------

;; negative-case tests are not snapshot targets (they must not parse)
(define (legal? name)
  (not (or (string-contains name "-errors")
           (string-contains name "-parse-errors"))))

;; "test/basic/closure.ta" -> "basic-closure.sexp" (flat naming)
(define (snap-name path)
  (let* ((parts (string-split path #\/))
         (dir (list-ref parts 1))
         (base (list-ref parts 2))
         (stem (string-take base (- (string-length base) 3))))
    (string-append dir "-" stem ".sexp")))

;; sorted list of regular-file names directly under dir
;; (guile 3.0 ftw callback: (full-path fileinfo-vector type-symbol); the
;; callback must return non-#f or ftw stops descending)
(define (list-entries dir)
  (let ((names '())
        (prefix (string-append dir "/")))
    (ftw dir (lambda (path info type)
               (when (and (string-prefix? prefix path)
                          (eqv? type 'regular))
                 (set! names (cons (basename path) names)))
               #t))
    (sort names string<?)))

;; sorted list of legal .ta paths under dir
(define (collect-files dir)
  (let loop ((names (list-entries dir)) (out '()))
    (if (null? names)
        (reverse out)
        (let* ((n (car names))
               (full (string-append dir "/" n)))
          (if (and (string-suffix? ".ta" n) (legal? n) (file-exists? full))
              (loop (cdr names) (cons full out))
              (loop (cdr names) out))))))

;; remove stale snapshots so the generated set is exactly this run's set
(define (clean-snap-dir)
  (when (file-exists? *snap-dir*)
    (for-each
     (lambda (n)
       (when (string-suffix? ".sexp" n)
         (delete-file (string-append *snap-dir* "/" n))))
     (list-entries *snap-dir*))))

;; --- ast-dump driver -----------------------------------------------------------

;; run ast-dump on src; returns the list (exit-code stdout stderr)
(define (run-ast-dump src)
  (let* ((cmd (string-append "./tinyactor run tools/kernfuzz/ast-dump.ta '"
                            src "' > '" *out-tmp* "' 2> '" *err-tmp*
                            "' ; echo $?"))
        (port (open-pipe cmd "r"))
        (code (string->number (string-trim (read-line port)))))
    (close-pipe port)
    (let ((stdout (if (file-exists? *out-tmp*)
                      (read-file-string *out-tmp*) ""))
          (stderr (if (file-exists? *err-tmp*)
                      (read-file-string *err-tmp*) "")))
      (when (file-exists? *out-tmp*) (delete-file *out-tmp*))
      (when (file-exists? *err-tmp*) (delete-file *err-tmp*))
      (list code stdout stderr))))

;; --- main -----------------------------------------------------------------------

(define (main)
  (unless (file-exists? "test/kernfuzz-frozen")
    (mkdir "test/kernfuzz-frozen" #o755))
  (unless (file-exists? *snap-dir*)
    (mkdir *snap-dir* #o755))
  (clean-snap-dir)

  (let ((failures '()) (ok 0) (scanned 0))
    (for-each
     (lambda (dir)
       (for-each
        (lambda (src)
          (set! scanned (+ scanned 1))
          (let* ((res (run-ast-dump src))
                 (code (car res))
                 (stdout (cadr res))
                 (stderr (caddr res)))
            (if (and (zero? code) (not (string=? stdout "")))
                (begin
                  (write-file-string (string-append *snap-dir* "/"
                                                    (snap-name src))
                                     stdout)
                  (set! ok (+ ok 1))
                  (display (string-append "  OK   " (basename src) "\n")))
                (let* ((src-line (if (not (string=? stderr ""))
                                     stderr
                                     stdout))
                       (line (clip (first-line src-line) 160)))
                  (set! failures
                        (cons (string-append src "\t"
                                             (number->string code)
                                             "\t" line)
                              failures))
                  (display (string-append "  FAIL " (basename src)
                                          " (exit " (number->string code)
                                          ")\n"))))))
                (collect-files dir)))
     *sources*)

    ;; expected-fail.txt: always rewritten (header + sorted entries)
        (let ((lines (sort (reverse failures) string<?)))
      (write-file-string
       *fail-path*
       (string-append
        (string-join
         (append
          (list "# expected-fail.txt — corpus files whose ast-dump fails"
                "# format: <file><TAB><exit_code><TAB><first error line>")
          (if (null? lines) '("# (none)") lines))
         "\n")
        "\n")
       )
      )

    (display "\n=== kernfuzz snapshot summary ===\n")
    (display (string-append "  scanned:       " (number->string scanned) "\n"))
    (display (string-append "  snapshotted:   " (number->string ok) "\n"))
    (display (string-append "  expected-fail: " (number->string (length failures)) "\n"))
    (display (string-append "  output:        " *snap-dir* "/\n")))
  (exit 0))

(main)