;; codegen.lisp — Lisp-syntax compiler from AST to .tabc bytecode
;; Translation of compile.c.
;;
;; State = (fn_names . (next_fn_id . (fn_entries . (env . (next_slot . max_slot)))))
;;   fn_names    = alist of (func_sym . fn_id) for top-level defines
;;   next_fn_id  = next fn_id counter for lambda allocation
;;   fn_entries  = alist of (fn_id . entry) for lambda function offsets
;;   env         = alist of (var_sym . slot)
;;   next_slot   = next available slot (negative)
;;   max_slot    = minimum slot used (for ENTER calculation)

;; ============================================================
;; Byte emission helpers
;; ============================================================

(define (emit_u32 b val)
  (let u (if (< val 0) (+ val 4294967296) val)
    (buf.push_byte b (% u 256))
    (buf.push_byte b (% (/ u 256) 256))
    (buf.push_byte b (% (/ u 65536) 256))
    (buf.push_byte b (% (/ u 16777216) 256))))

(define (emit_i64 b val)
  (buf.push_byte b (% val 256))
  (buf.push_byte b (% (/ val 256) 256))
  (buf.push_byte b (% (/ val 65536) 256))
  (buf.push_byte b (% (/ val 16777216) 256))
  (let hi (/ val 4294967296)
    (buf.push_byte b (% hi 256))
    (buf.push_byte b (% (/ hi 256) 256))
    (buf.push_byte b (% (/ hi 65536) 256))
    (buf.push_byte b (% (/ hi 16777216) 256))))

(define (patch_u32 b pos val)
  (let u (if (< val 0) (+ val 4294967296) val)
    (buf.set_byte b pos (% u 256))
    (buf.set_byte b (+ pos 1) (% (/ u 256) 256))
    (buf.set_byte b (+ pos 2) (% (/ u 65536) 256))
    (buf.set_byte b (+ pos 3) (% (/ u 16777216) 256))))

;; ============================================================
(define (mk_true) (< 0 1))
(define (mk_false) (< 1 0))

;; Symbol table (for .tabc serialization)
;; ============================================================

(define (init_syms)
  (cons (cons "quote" 0) (cons (cons "define" 1) (cons (cons "lambda" 2)
  (cons (cons "if" 3) (cons (cons "begin" 4) (cons (cons "let" 5)
  (cons (cons "letrec" 6) (cons (cons "match" 7) (cons (cons "spawn" 8)
  (cons (cons "send" 9) (cons (cons "recv" 10) (cons (cons "self" 11)
  (cons (cons "monitor" 12) (cons (cons "cons" 13) (cons (cons "car" 14)
  (cons (cons "cdr" 15) (cons (cons "+" 16) (cons (cons "-" 17)
  (cons (cons "*" 18) (cons (cons "/" 19) (cons (cons "%" 20)
  (cons (cons "=" 21) (cons (cons "<" 22) (cons (cons "<=" 23)
  (cons (cons ">" 24) (cons (cons ">=" 25) (cons (cons "null?" 26)
  (cons (cons "pair?" 27) (cons (cons "int?" 28) (cons (cons "string?" 29)
  (cons (cons "bytes?" 30) (cons (cons "pid?" 31) (cons (cons "print" 32)
  (cons (cons "true" 33) (cons (cons "false" 34) (cons (cons "DOWN" 35)
  (cons (cons "nil" 36) (cons (cons "_" 37) (cons (cons "and" 38)
  (cons (cons "or" 39) (cons (cons "not" 40) (cons (cons "set!" 41)
    'nil)))))))))))))))))))))))))))))))))))))))))))

(define (sym_count syms)
  (if (null? syms) 0 (+ 1 (sym_count (cdr syms)))))

(define (sym_find_by_idx syms idx)
  (if (null? syms) ""
    (if (= (cdr (car syms)) idx)
        (car (car syms))
        (sym_find_by_idx (cdr syms) idx))))

(define (find_sym_index syms sym)
  (if (null? syms) 'nil
      (if (= (str.to_sym (car (car syms))) sym)
          (cdr (car syms))
          (find_sym_index (cdr syms) sym))))

;; Add a new symbol to the symbol table, returns (new_syms . idx)
(define (intern_ast_sym syms sym next_idx)
  (let idx (find_sym_index syms sym)
    (if (null? idx)
        (cons (cons (cons (str.sym_to_str sym) next_idx) syms)
              (+ next_idx 1))
        (cons syms next_idx))))

;; Store symbol table in fn_names alist under special key
(define (embed_syms fn_names syms)
  (cons (cons '__syms__ syms) fn_names))

(define (get_syms fn_names)
  (let s (alist_find_sym fn_names '__syms__)
    (if (null? s) (init_syms) s)))

;; Walk AST and collect all symbols in source order
(define (collect_ast_syms ast syms next_idx)
  (if (null? ast) (cons syms next_idx)
      (if (int? ast) (cons syms next_idx)
          (if (string? ast) (cons syms next_idx)
              (if (pair? ast)
                  (let r1 (collect_ast_syms (car ast) syms next_idx)
                    (collect_ast_syms (cdr ast) (car r1) (cdr r1)))
                  ;; Skip true/false tagged values
                  (if (= ast (mk_true)) (cons syms next_idx)
                      (if (= ast (mk_false)) (cons syms next_idx)
                          (intern_ast_sym syms ast next_idx))))))))

(define (collect_ast_list forms syms next_idx)
  (if (null? forms) (cons syms next_idx)
      (let r (collect_ast_syms (car forms) syms next_idx)
        (collect_ast_list (cdr forms) (car r) (cdr r)))))

;; ============================================================
;; List/alist helpers
;; ============================================================

(define (alist_find_sym lst key)
  (if (null? lst) 'nil
    (if (= (car (car lst)) key)
        (cdr (car lst))
        (alist_find_sym (cdr lst) key))))

(define (list_len lst)
  (if (null? lst) 0 (+ 1 (list_len (cdr lst)))))

(define (cg_list_ref lst n)
  (if (= n 0) (car lst)
      (cg_list_ref (cdr lst) (- n 1))))

(define (cg_reverse_list lst acc)
  (if (null? lst) acc
      (cg_reverse_list (cdr lst) (cons (car lst) acc))))

(define (is_define_form head)
  (if (= head 'define) 1
      (if (= head 'define_pub) 1 0)))

(define (rindex_dot s len)
  (if (= len 0) (- 0 1)
      (if (= (str.char_at s (- len 1)) 46)
          (- len 1)
          (rindex_dot s (- len 1)))))

(define (find_fn fn_names sym)
  (let direct (alist_find_sym fn_names sym)
    (if (null? direct)
        (let s (str.sym_to_str sym)
          (let dot (rindex_dot s (str.length s))
            (if (= dot (- 0 1))
                'nil
                (let base (str.substr s (+ dot 1) (- (str.length s) (+ dot 1)))
                  (let base_sym (str.to_sym base)
                    (alist_find_sym fn_names base_sym))))))
        direct)))

;; ============================================================
;; State accessors
;; ============================================================

(define (st_fn_names s) (car s))
(define (st_next_fn_id s) (car (cdr s)))
(define (st_fn_entries s) (car (cdr (cdr s))))
(define (st_env s) (car (cdr (cdr (cdr s)))))
(define (st_next_slot s) (car (cdr (cdr (cdr (cdr s))))))
(define (st_max_slot s) (cdr (cdr (cdr (cdr (cdr s))))))

(define (make_state fn_names next_fn_id fn_entries env next_slot max_slot)
  (cons fn_names (cons next_fn_id (cons fn_entries
    (cons env (cons next_slot max_slot))))))

;; ============================================================
;; Inline operator lookup
;; ============================================================

(define (inline_op sym)
  (if (= sym '+) 12
  (if (= sym '-) 13
  (if (= sym '*) 14
  (if (= sym '/) 15
  (if (= sym '%) 16
  (if (= sym '=) 17
  (if (= sym '<) 18
  (if (= sym '<=) 19
  (if (= sym 'cons) 9
  (if (= sym 'car) 10
  (if (= sym 'cdr) 11
  (if (= sym 'null?) 20
  (if (= sym 'pair?) 21
  (if (= sym 'int?) 22
  (if (= sym 'string?) 23
  (if (= sym 'bytes?) 24
  (if (= sym 'pid?) 25
  (if (= sym 'print) 43 -1)))))))))))))))))))

;; > uses OP_LT with swapped operands, >= uses OP_LE with swapped operands
(define (inline_swap_op sym)
  (if (= sym '>) 18
  (if (= sym '>=) 19 -1)))

(define (is_symbol_val v)
  (if (int? v) 0
  (if (string? v) 0
  (if (pair? v) 0
  (if (null? v) 0 1)))))

;; ============================================================
;; Free variable collection
;; ============================================================

(define (fv_contains fv name)
  (if (null? fv) 0
      (if (= (car (car fv)) name) 1
          (fv_contains (cdr fv) name))))

(define (fv_add name slot fv)
  (if (= (fv_contains fv name) 1) fv
      (cons (cons name slot) fv)))

(define (is_param_sym sym params)
  (if (null? params) 0
      (if (= (car params) sym) 1
          (is_param_sym sym (cdr params)))))

(define (filter_free_vars fv params)
  (if (null? fv) 'nil
      (if (= (is_param_sym (car (car fv)) params) 1)
          (filter_free_vars (cdr fv) params)
          (cons (car fv) (filter_free_vars (cdr fv) params)))))

(define (collect_free_vars expr env acc)
  (if (int? expr) acc
  (if (string? expr) acc
  (if (null? expr) acc
  (if (pair? expr)
      (let head (car expr)
        (if (= (is_symbol_val head) 0)
            (collect_free_list expr env acc)
            (if (= head 'quote) acc
            (if (= head 'define) acc
            (if (= head 'lambda)
                (collect_free_body (cdr (cdr expr)) env acc)
            (if (= head 'if)
                (let a1 (collect_free_vars (cg_list_ref expr 1) env acc)
                  (let a2 (collect_free_vars (cg_list_ref expr 2) env a1)
                    (collect_free_vars (cg_list_ref expr 3) env a2)))
            (if (= head 'begin)
                (collect_free_body (cdr expr) env acc)
            (if (= head 'let)
                (let second (cg_list_ref expr 1)
                  (if (= (is_symbol_val second) 1)
                      (let a1 (collect_free_vars (cg_list_ref expr 2) env acc)
                        (collect_free_body (cdr (cdr (cdr expr))) env a1))
                      (if (= (is_symbol_val second) 0)
                          (collect_free_bindings second env acc)
                          acc)))
            (if (= head 'match)
                (let a1 (collect_free_vars (cg_list_ref expr 1) env acc)
                  (collect_free_branches (cdr (cdr expr)) env a1))
            (if (= head 'receive)
                (collect_free_branches (cdr expr) env acc)
            (if (= head 'receive-scan)
                (let lam (cg_list_ref expr 1)
                  (if (= (is_symbol_val lam) 0)
                      (collect_free_body (cdr (cdr lam)) env acc)
                      acc))
                        (let a0 (collect_free_vars head env acc)
              (collect_free_list (cdr expr) env a0)))))))))))))
      (let slot (alist_find_sym env expr)
        (if (null? slot) acc
            (fv_add expr slot acc))))))))

(define (collect_free_list lst env acc)
  (if (null? lst) acc
      (collect_free_list (cdr lst) env
        (collect_free_vars (car lst) env acc))))

(define (collect_free_body body env acc)
  (if (null? body) acc
      (collect_free_body (cdr body) env
        (collect_free_vars (car body) env acc))))

(define (collect_free_bindings bindings env acc)
  (if (null? bindings) acc
      (let binding (car bindings)
        (collect_free_bindings (cdr bindings) env
          (collect_free_vars (cg_list_ref binding 1) env acc)))))

(define (collect_free_branches branches env acc)
  (if (null? branches) acc
      (collect_free_branches (cdr branches) env
        (collect_free_body (cdr (car branches)) env acc))))

;; ============================================================
;; Patch helpers
;; ============================================================

(define (patch_jumps b jumps target)
  (if (null? jumps) 'nil
      (begin (patch_u32 b (car jumps) target)
             (patch_jumps b (cdr jumps) target))))

;; ============================================================
;; Env building helpers
;; ============================================================

(define (build_param_env params slot)
  (if (null? params) 'nil
      (cons (cons (car params) slot)
            (build_param_env (cdr params) (+ slot 1)))))

(define (build_combined_env params slot outer_env)
  (if (null? params) outer_env
      (cons (cons (car params) slot)
            (build_combined_env (cdr params) (+ slot 1) outer_env))))

(define (add_free_vars_to_env fv slot env)
  (if (null? fv) env
      (add_free_vars_to_env (cdr fv) (+ slot 1)
        (cons (cons (car (car fv)) slot) env))))

(define (build_body_env params free_vars)
  (build_body_env_loop params 0 free_vars))

(define (build_body_env_loop params slot free_vars)
  (if (null? params)
      (add_free_vars_to_env free_vars slot 'nil)
      (cons (cons (car params) slot)
            (build_body_env_loop (cdr params) (+ slot 1) free_vars))))

(define (emit_free_slots b fv)
  (if (null? fv) 'nil
      (begin (emit_u32 b (cdr (car fv)))
             (emit_free_slots b (cdr fv)))))

;; ============================================================
;; Expression compilation
;; ============================================================

(define (compile_expr b expr tail state)
  (if (int? expr)
      (begin
        (if (>= expr 0)
            (if (< expr 128)
                (begin (buf.push_byte b 3) (buf.push_byte b expr))
                (begin (buf.push_byte b 4) (emit_i64 b expr)))
            (if (>= expr -128)
                (begin (buf.push_byte b 3) (buf.push_byte b (+ expr 256)))
                (begin (buf.push_byte b 4) (emit_i64 b expr))))
        state)
      (if (string? expr)
          (begin
            (buf.push_byte b 6)
            (emit_u32 b (str.length expr))
            (buf.push_string b expr)
            state)
          (if (null? expr)
              (begin (buf.push_byte b 0) state)
                                                        (if (pair? expr)
                  (compile_list b expr tail state)
                  ;; Symbol: check env, then fn_names
                  (let env (st_env state)
                    (let slot (alist_find_sym env expr)
                      (if (null? slot)
                          (let fn_names (st_fn_names state)
                            (let fid (find_fn fn_names expr)
                                                                                          (if (null? fid)
                                  (if (= expr (mk_true))
                                      (begin (buf.push_byte b 1) state)
                                      (if (= expr (mk_false))
                                          (begin (buf.push_byte b 2) state)
                                          (begin (buf.push_byte b 0) state)))
                                  (begin (buf.push_byte b 30)
                                         (emit_u32 b fid)
                                         (emit_u32 b 0)
                                         state))))
                          (begin (buf.push_byte b 7)
                                                                  (emit_u32 b slot)
                                                           state)))))))))

(define (compile_list b expr tail state)
  (let head (car expr)
    (if (= head 'if)
        (compile_if b (cdr expr) tail state)
        (if (= head 'let)
            (compile_let b (cdr expr) tail state)
            (if (= head 'begin)
                (compile_begin b (cdr expr) tail state)
                (if (= head 'lambda)
                    (compile_lambda b (cdr expr) state)
                    (if (= head 'and)
                        (compile_and b (cdr expr) tail state)
                        (if (= head 'or)
                            (compile_or b (cdr expr) tail state)
                            (if (= head 'quote)
                                (compile_quote b (cdr expr) state)
                                (if (= head 'define)
                                    (begin (buf.push_byte b 0) state)
                                    (if (= head 'import)
                                        (begin (buf.push_byte b 0) state)
                                        (if (= head 'type)
                                                                                        (begin (buf.push_byte b 0) state)
                                            (compile_call b head (cdr expr) tail state)))))))))))))

;; ============================================================
;; (if cond then else?)
;; ============================================================

(define (compile_if b args tail state)
  (let s1 (compile_expr b (car args) 0 state)
    (buf.push_byte b 27)
    (let jif_pos (buf.length b)
      (emit_u32 b 0)
      (let s2 (compile_expr b (car (cdr args)) tail s1)
        (if (>= (list_len args) 3)
            (begin
              (buf.push_byte b 26)
              (let j_pos (buf.length b)
                (emit_u32 b 0)
                (let else_start (buf.length b)
                  (patch_u32 b jif_pos else_start)
                  (let s3 (compile_expr b (car (cdr (cdr args))) tail s2)
                    (patch_u32 b j_pos (buf.length b))
                    s3))))
            (begin
              (patch_u32 b jif_pos (buf.length b))
              (buf.push_byte b 0)
              s2))))))

;; ============================================================
;; (let var init body...) or (let ((v e)...) body...)
;; ============================================================

(define (compile_let b args tail state)
  (let second (car args)
    (if (= (is_symbol_val second) 1)
        (compile_let_single b second (car (cdr args)) (cdr (cdr args)) tail state)
        (if (null? second)
            (compile_body b (cdr args) tail state)
            (compile_let_multi b second (cdr args) tail state)))))

(define (compile_let_single b var init body tail state)
  (let s1 (compile_expr b init 0 state)
    (let ns (st_next_slot s1)
      (let ms (st_max_slot s1)
        (let new_ms (if (< (- ns 1) ms) (- ns 1) ms)
          (buf.push_byte b 8)
          (emit_u32 b ns)
          (let new_env (cons (cons var ns) (st_env s1))
            (compile_body b body tail
              (make_state (st_fn_names s1) (st_next_fn_id s1)
                          (st_fn_entries s1) new_env (- ns 1) new_ms))))))))

(define (compile_let_multi b bindings body tail state)
  (if (null? bindings)
      (compile_body b body tail state)
      (let binding (car bindings)
        (let vname (car binding)
          (let init_expr (car (cdr binding))
            (let s1 (compile_expr b init_expr 0 state)
                            (let ns (st_next_slot s1)
                (let ms (st_max_slot s1)
                  (let new_ms (if (< (- ns 1) ms) (- ns 1) ms)
                    (buf.push_byte b 8)
                    (emit_u32 b ns)
                    (let new_env (cons (cons vname ns) (st_env s1))
                      (compile_let_multi b (cdr bindings) body tail
                        (make_state (st_fn_names s1) (st_next_fn_id s1)
                                    (st_fn_entries s1) new_env (- ns 1) new_ms))))))))))))

;; ============================================================
;; (begin e1 e2 ... eN)
;; ============================================================

(define (compile_begin b args tail state)
  (if (null? args)
      (begin (buf.push_byte b 0) state)
      (compile_begin_loop b args tail state)))

(define (compile_begin_loop b args tail state)
  (if (null? (cdr args))
      (compile_expr b (car args) tail state)
      (let s1 (compile_expr b (car args) 0 state)
        (buf.push_byte b 28)
        (compile_begin_loop b (cdr args) tail s1))))

;; ============================================================
;; (and e1 e2 ... eN) — short-circuit
;; ============================================================

(define (compile_and b args tail state)
  (if (null? args)
      (begin (buf.push_byte b 1) state)
      (if (null? (cdr args))
          (compile_expr b (car args) tail state)
          (compile_and_loop b args tail state 'nil))))

(define (compile_and_loop b args tail state jumps)
  (if (null? (cdr args))
      (let s (compile_expr b (car args) tail state)
        (patch_jumps b jumps (buf.length b))
        s)
      (let s (compile_expr b (car args) 0 state)
        (buf.push_byte b 29)
        (buf.push_byte b 27)
        (let jpos (buf.length b)
          (emit_u32 b 0)
          (buf.push_byte b 28)
          (compile_and_loop b (cdr args) tail s (cons jpos jumps))))))

;; ============================================================
;; (or e1 e2 ... eN) — short-circuit
;; ============================================================

(define (compile_or b args tail state)
  (if (null? args)
      (begin (buf.push_byte b 0) state)
      (if (null? (cdr args))
          (compile_expr b (car args) tail state)
          (compile_or_loop b args tail state 'nil))))

(define (compile_or_loop b args tail state end_jumps)
  (if (null? (cdr args))
      (let s (compile_expr b (car args) tail state)
        (patch_jumps b end_jumps (buf.length b))
        s)
      (let s (compile_expr b (car args) 0 state)
        (buf.push_byte b 29)
        (buf.push_byte b 27)
        (let patch_next (buf.length b)
          (emit_u32 b 0)
          (buf.push_byte b 26)
          (let end_pos (buf.length b)
            (emit_u32 b 0)
            (patch_u32 b patch_next (buf.length b))
            (buf.push_byte b 28)
            (compile_or_loop b (cdr args) tail s (cons end_pos end_jumps)))))))

;; ============================================================
;; (quote x)
;; ============================================================

(define (compile_quote b args state)
    (let quoted (car args)
    (if (null? quoted)
        (begin (buf.push_byte b 0) state)
        (if (int? quoted)
            (begin
              (if (>= quoted 0)
                  (if (< quoted 128)
                      (begin (buf.push_byte b 3) (buf.push_byte b quoted))
                      (begin (buf.push_byte b 4) (emit_i64 b quoted)))
                  (if (>= quoted -128)
                      (begin (buf.push_byte b 3) (buf.push_byte b (+ quoted 256)))
                      (begin (buf.push_byte b 4) (emit_i64 b quoted))))
              state)
            (if (pair? quoted)
                (begin (buf.push_byte b 0) state)
                (begin
                  (let idx (find_sym_index (get_syms (st_fn_names state)) quoted))
                  (buf.push_byte b 5)
                  (emit_u32 b idx)
                  state))))))

;; ============================================================
;; (lambda (params...) body...)
;; ============================================================

(define (compute_free_vars body env params)
  (let fv_raw (collect_free_body body env 'nil)
    (let fv_filtered (filter_free_vars fv_raw params)
      (cg_reverse_list fv_filtered 'nil))))

(define (compile_lambda b args state)
  (let params (car args)
    (let body (cdr args)
      (let fn_names (st_fn_names state)
        (let next_fn_id (st_next_fn_id state)
          (let fn_entries (st_fn_entries state)
            (let env (st_env state)
              (let combined_env (build_combined_env params 0 env)
                (let free_vars (compute_free_vars body combined_env params)
                  (let nfree (list_len free_vars)
                    (buf.push_byte b 30)
                    (emit_u32 b next_fn_id)
                    (emit_u32 b nfree)
                    (emit_free_slots b free_vars)
                    (buf.push_byte b 26)
                    (let jump_pos (buf.length b)
                      (emit_u32 b 0)
                      (let entry (buf.length b)
                        (let body_env (build_body_env params free_vars)
                          (buf.push_byte b 55)
                          (let enter_pos (buf.length b)
                            (emit_u32 b 0)
                            (let body_state (make_state fn_names (+ next_fn_id 1)
                                (cons (cons next_fn_id entry) fn_entries)
                                body_env -5 -5)
                              (let fs (compile_body b body 1 body_state)
                                (buf.push_byte b 33)
                                (patch_u32 b enter_pos (- -5 (st_max_slot fs)))
                                (patch_u32 b jump_pos (buf.length b))
                                                                (make_state fn_names (st_next_fn_id fs)
                                            (st_fn_entries fs)
                                            env (st_next_slot state)
                                            (st_max_slot fs))))))))))))))))))

;; ============================================================
;; Function call compilation
;; ============================================================

(define (compile_call b head args tail state)
  (let op (inline_op head)
    (if (>= op 0)
        (begin
          (compile_args b args state)
          (buf.push_byte b op)
          state)
        (let swap_op (inline_swap_op head)
          (if (>= swap_op 0)
              (let s1 (compile_expr b (cg_list_ref args 1) 0 state)
                (let s2 (compile_expr b (cg_list_ref args 0) 0 s1)
                  (buf.push_byte b swap_op)
                  s2))
              (if (= (is_symbol_val head) 1)
                  (let cfidx (vm.cfunc_index head)
                    (if (>= cfidx 0)
                        (begin
                          (compile_args b args state)
                          (buf.push_byte b 54)
                          (emit_u32 b cfidx)
                          (buf.push_byte b (list_len args))
                          state)
                        (compile_special b head args tail state)))
                  (compile_special b head args tail state)))))))

(define (patch_all_jumps b jumps addr)
  (if (null? jumps) 'nil
      (begin (patch_u32 b (car jumps) addr)
             (patch_all_jumps b (cdr jumps) addr))))

(define (is_wildcard_sym v)
  (if (= (is_symbol_val v) 1)
      (if (= (str.eq (str.sym_to_str v) "_") 1) 1 0)
      0))

(define (is_pattern_var v)
  (if (= (is_symbol_val v) 1)
      (if (= (is_wildcard_sym v) 1) 0
          (if (= (str.eq (str.sym_to_str v) "nil") 1) 0
              (if (= (str.eq (str.sym_to_str v) "cons") 1) 0
                  (if (= (str.eq (str.sym_to_str v) "quote") 1) 0 1))))
      0))

;; Helper: allocate a car/cdr pair from subj_slot via MATCH_PAIR.
;; Emits OP_LOAD, OP_MATCH_PAIR, OP_MATCH_JUMP, then OP_STORE car/cdr.
;; Returns (state . (car_slot . (cdr_slot . fail_jumps))).
(define (emit_match_pair b subj_slot state fail_jumps)
  (buf.push_byte b 7)
  (emit_u32 b subj_slot)
  (buf.push_byte b 48)
  (buf.push_byte b 49)
  (let jmp_pos (buf.length b)
    (emit_u32 b 0)
    (let new_fj (cons jmp_pos fail_jumps)
      (let car_slot (st_next_slot state)
        (let cdr_slot (- car_slot 1)
          (let new_next (- cdr_slot 1)
            (buf.push_byte b 8)
            (emit_u32 b car_slot)
            (buf.push_byte b 8)
            (emit_u32 b cdr_slot)
            (let state2 (make_state (st_fn_names state) (st_next_fn_id state)
                                     (st_fn_entries state) (st_env state)
                                     new_next (if (< new_next (st_max_slot state)) new_next (st_max_slot state))))
              (cons state2 (cons car_slot (cons cdr_slot new_fj))))))))))

;; Compile a list pattern (p1 p2 ... pN) as nested pair destructuring.
(define (compile_list_pattern_loop b remaining subj_slot state fail_jumps)
  (if (null? remaining)
      (begin
        (buf.push_byte b 7)
        (emit_u32 b subj_slot)
        (buf.push_byte b 47)
        (buf.push_byte b 49)
        (let jmp_pos (buf.length b)
          (emit_u32 b 0)
          (cons state (cons jmp_pos fail_jumps))))
      (let pr (emit_match_pair b subj_slot state fail_jumps)
        (let state2 (car pr)
          (let car_slot (car (cdr pr))
            (let cdr_slot (car (cdr (cdr pr)))
              (let fj2 (cdr (cdr (cdr pr)))
                (let r1 (compile_pattern b (car remaining) car_slot state2 fj2)
                  (compile_list_pattern_loop b (cdr remaining) cdr_slot (car r1) (cdr r1))))))))))

(define (compile_pattern b pat subj_slot state fail_jumps)
  (if (= (is_wildcard_sym pat) 1)
      (cons state fail_jumps)
      (if (= (is_pattern_var pat) 1)
          (let var_slot (st_next_slot state)
            (buf.push_byte b 7)
            (emit_u32 b subj_slot)
            (buf.push_byte b 8)
            (emit_u32 b var_slot)
            (let new_env (cons (cons pat var_slot) (st_env state))
              (let state2 (make_state (st_fn_names state) (st_next_fn_id state)
                                       (st_fn_entries state) new_env
                                       (- var_slot 1) (if (< (- var_slot 1) (st_max_slot state)) (- var_slot 1) (st_max_slot state)))
                (cons state2 fail_jumps))))
          (if (int? pat)
              (begin
                (buf.push_byte b 7)
                (emit_u32 b subj_slot)
                (buf.push_byte b 45)
                (emit_i64 b pat)
                (buf.push_byte b 49)
                (let jmp_pos (buf.length b)
                  (emit_u32 b 0)
                  (cons state (cons jmp_pos fail_jumps))))
              (if (null? pat)
                  (begin
                    (buf.push_byte b 7)
                    (emit_u32 b subj_slot)
                    (buf.push_byte b 47)
                    (buf.push_byte b 49)
                    (let jmp_pos (buf.length b)
                      (emit_u32 b 0)
                      (cons state (cons jmp_pos fail_jumps))))
                  (if (pair? pat)
                      (let head (car pat)
                        (if (= head 'quote)
                            (let quoted (car (cdr pat))
                              (if (= (is_symbol_val quoted) 1)
                                  (begin
                                    (buf.push_byte b 7)
                                    (emit_u32 b subj_slot)
                                    (buf.push_byte b 46)
                                    (emit_u32 b (find_sym_index (get_syms (st_fn_names state)) quoted))
                                    (buf.push_byte b 49)
                                    (let jmp_pos (buf.length b)
                                      (emit_u32 b 0)
                                      (cons state (cons jmp_pos fail_jumps))))
                                  (if (null? quoted)
                                      (begin
                                        (buf.push_byte b 7)
                                        (emit_u32 b subj_slot)
                                        (buf.push_byte b 47)
                                        (buf.push_byte b 49)
                                        (let jmp_pos (buf.length b)
                                          (emit_u32 b 0)
                                          (cons state (cons jmp_pos fail_jumps))))
                                      (cons state fail_jumps))))
                            (if (= head 'cons)
                                (let pr (emit_match_pair b subj_slot state fail_jumps)
                                  (let state2 (car pr)
                                    (let car_slot (car (cdr pr))
                                      (let cdr_slot (car (cdr (cdr pr)))
                                        (let fj2 (cdr (cdr (cdr pr)))
                                          (let pat_a (car (cdr pat))
                                            (let pat_b (car (cdr (cdr pat)))
                                              (let r1 (compile_pattern b pat_a car_slot state2 fj2)
                                                (compile_pattern b pat_b cdr_slot (car r1) (cdr r1))))))))))
                                (compile_list_pattern_loop b pat subj_slot state fail_jumps))))
                      (cons state fail_jumps)))))))

(define (compile_match_branches b branches tail state subj_slot end_jumps)
  (if (null? branches)
      (begin
        (buf.push_byte b 0)
        (patch_all_jumps b end_jumps (buf.length b))
        state)
      (let branch (car branches)
        (let pat (car branch)
          (let body (car (cdr branch))
            (let saved_slot (st_next_slot state)
              (let saved_env (st_env state)
                (let pj (compile_pattern b pat subj_slot state 'nil)
                  (let state2 (car pj)
                    (let fail_jumps (cdr pj)
                      (let s3 (compile_expr b body tail state2)
                        (buf.push_byte b 26)
                        (let jmp_pos (buf.length b)
                          (emit_u32 b 0)
                          (patch_all_jumps b fail_jumps (buf.length b))
                          (let state3 (make_state (st_fn_names s3) (st_next_fn_id s3)
                                                    (st_fn_entries s3) saved_env
                                                    saved_slot (st_max_slot s3))
                            (compile_match_branches b (cdr branches) tail state3 subj_slot
                              (cons jmp_pos end_jumps)))))))))))))))

(define (compile_match b args tail state)
  (let scrutinee (car args)
    (let branches (cdr args)
      (let s1 (compile_expr b scrutinee 0 state)
        (let subj_slot (st_next_slot s1)
          (buf.push_byte b 8)
          (emit_u32 b subj_slot)
                    (let s2 (make_state (st_fn_names s1) (st_next_fn_id s1) (st_fn_entries s1)
                              (st_env s1) (- subj_slot 1) (if (< (- subj_slot 1) (st_max_slot s1)) (- subj_slot 1) (st_max_slot s1)))
            (compile_match_branches b branches tail s2 subj_slot 'nil)))))))

(define (compile_receive_branches b branches tail state subj_slot loop_start end_jumps)
  (if (null? branches)
      (begin
        (buf.push_byte b 26)
        (emit_u32 b loop_start)
        (patch_all_jumps b end_jumps (buf.length b))
        state)
      (let branch (car branches)
        (let pat (car branch)
          (let body (car (cdr branch))
            (let saved_slot (st_next_slot state)
              (let saved_env (st_env state)
                (let pj (compile_pattern b pat subj_slot state 'nil)
                  (let state2 (car pj)
                    (let fail_jumps (cdr pj)
                      (buf.push_byte b 40)
                      (let s3 (compile_expr b body tail state2)
                        (buf.push_byte b 26)
                        (let jmp_pos (buf.length b)
                          (emit_u32 b 0)
                          (patch_all_jumps b fail_jumps (buf.length b))
                          (let state3 (make_state (st_fn_names s3) (st_next_fn_id s3)
                                                    (st_fn_entries s3) saved_env
                                                    saved_slot (st_max_slot s3))
                            (compile_receive_branches b (cdr branches) tail state3 subj_slot
                              loop_start (cons jmp_pos end_jumps)))))))))))))))

(define (compile_receive b args tail state)
  (let branches args
    (let subj_slot (st_next_slot state)
      (let new_next (- subj_slot 1)
        (let state2 (make_state (st_fn_names state) (st_next_fn_id state) (st_fn_entries state)
                                  (st_env state) new_next (if (< new_next (st_max_slot state)) new_next (st_max_slot state))))
          (let loop_start (buf.length b)
            (buf.push_byte b 39)
            (buf.push_byte b 8)
            (emit_u32 b subj_slot)
            (compile_receive_branches b branches tail state2 subj_slot loop_start 'nil)))))))

(define (compile_special b head args tail state)
  (if (= head 'spawn)
      (compile_spawn b args state)
      (if (= head 'send)
          (compile_send b args state)
          (if (= head 'recv)
              (begin (buf.push_byte b 38) state)
              (if (= head 'self)
                  (begin (buf.push_byte b 41) state)
                  (if (= head 'monitor)
                      (let s1 (compile_expr b (car args) 0 state)
                        (buf.push_byte b 42)
                        s1)
                                            (if (= head 'match)
                          (compile_match b args tail state)
                          (if (= head 'receive)
                              (compile_receive b args tail state)
                              (compile_general_call b head args tail state)))))))))

(define (compile_spawn b args state)
  (let arg0 (car args)
    (if (pair? arg0)
        (if (= (car arg0) 'quote)
            (let quoted (car (cdr arg0))
              (if (= (is_symbol_val quoted) 1)
                                    (let fid_raw (find_fn (st_fn_names state) quoted)
                    (let fid (if (null? fid_raw) 0 fid_raw)
                      (buf.push_byte b 34)
                      (emit_u32 b fid)
                      state))
                  (let s1 (compile_expr b arg0 0 state)
                    (buf.push_byte b 36)
                    s1)))
            (let s1 (compile_expr b arg0 0 state)
              (buf.push_byte b 36)
              s1))
        (let s1 (compile_expr b arg0 0 state)
          (buf.push_byte b 36)
          s1))))

(define (compile_send b args state)
  ;; compile.c: compile msg, compile pid, OP_SEND
  (let s1 (compile_expr b (car (cdr args)) 0 state)
    (let s2 (compile_expr b (car args) 0 s1)
      (buf.push_byte b 37)
      s2)))

(define (compile_general_call b head args tail state)
  (let s1 (compile_expr b head 0 state)
    (let s2 (compile_args b args s1)
      (buf.push_byte b (if (= tail 1) 32 31))
      (emit_u32 b (list_len args))
      s2)))

(define (compile_args b args state)
  (if (null? args) state
      (compile_args b (cdr args)
        (compile_expr b (car args) 0 state))))

;; ============================================================
;; receive-scan special form
;; (receive-scan (lambda (msg) body...))
;; ============================================================

(define (compile_receive_scan b args tail state)
  (let lam (car args)
    (let params (car (cdr lam))
      (let body (cdr (cdr lam))
                (let msg_sym (car params)
          (let ns (st_next_slot state)
            (let ms (st_max_slot state)
              (let new_ms (if (< (- ns 1) ms) (- ns 1) ms)
                (let loop_start (buf.length b)
                  (buf.push_byte b 39)
                  (buf.push_byte b 8)
                  (emit_u32 b ns)
                  (let new_env (cons (cons msg_sym ns) (st_env state))
                    (let body_state (make_state (st_fn_names state)
                        (st_next_fn_id state) (st_fn_entries state)
                        new_env (- ns 1) new_ms)
                      (let fs (compile_body b body 0 body_state)
                        (buf.push_byte b 26)
                        (emit_u32 b loop_start)
                        fs))))))))))))

;; ============================================================
;; Body compilation
;; ============================================================

(define (compile_body b body tail state)
  (if (null? body)
      (begin (buf.push_byte b 0) state)
      (if (null? (cdr body))
          (compile_expr b (car body) tail state)
          (let s1 (compile_expr b (car body) 0 state)
            (buf.push_byte b 28)
            (compile_body b (cdr body) tail s1)))))

;; ============================================================
;; Define body compilation
;; ============================================================

(define (compile_define_body b fn_names next_fn_id fn_entries sig body)
  (let param_env (build_param_env (cdr sig) 0)
    (buf.push_byte b 55)
    (let enter_pos (buf.length b)
      (emit_u32 b 0)
      (let init_state (make_state fn_names next_fn_id fn_entries param_env -5 -5)
        (let fs (compile_body b body 1 init_state)
          (buf.push_byte b 33)
          (patch_u32 b enter_pos (- -5 (st_max_slot fs)))
          fs)))))

;; ============================================================
;; Pass 1: register function names
;; ============================================================

(define (pass1_register forms acc next_id)
  (if (null? forms) (cons acc next_id)
      (let form (car forms)
        (if (pair? form)
            (if (= (is_define_form (car form)) 1)
                (let sig (car (cdr form))
                  (if (pair? sig)
                      (pass1_register (cdr forms)
                        (cons (cons (car sig) next_id) acc)
                        (+ next_id 1))
                      (pass1_register (cdr forms) acc next_id)))
                (pass1_register (cdr forms) acc next_id))
            (pass1_register (cdr forms) acc next_id)))))

;; ============================================================
;; Pass 2: compile function bodies
;; ============================================================

(define (pass2_compile forms b fn_names next_fn_id fn_entries cur_id acc)
  (if (null? forms)
      (cons next_fn_id (cons fn_entries acc))
      (let form (car forms)
        (if (pair? form)
            (if (= (is_define_form (car form)) 1)
                (let sig (car (cdr form))
                  (if (pair? sig)
                      (let entry (buf.length b)
                        (let fs (compile_define_body b fn_names next_fn_id fn_entries sig (cdr (cdr form)))
                          (pass2_compile (cdr forms) b fn_names
                            (st_next_fn_id fs) (st_fn_entries fs)
                            (+ cur_id 1)
                            (cons (cons cur_id entry) acc))))
                      (pass2_compile (cdr forms) b fn_names next_fn_id fn_entries cur_id acc)))
                (pass2_compile (cdr forms) b fn_names next_fn_id fn_entries cur_id acc))
            (pass2_compile (cdr forms) b fn_names next_fn_id fn_entries cur_id acc)))))

;; ============================================================
;; Lookup helpers
;; ============================================================

(define (find_fn_offset offsets fn_id)
  (if (null? offsets) 0
      (if (= (car (car offsets)) fn_id)
          (cdr (car offsets))
          (find_fn_offset (cdr offsets) fn_id))))

(define (find_fn_entry define_offsets lambda_offsets fn_id)
  (let off (find_fn_offset define_offsets fn_id)
    (if (> off 0) off
        (find_fn_offset lambda_offsets fn_id))))

(define (find_main_fn fn_names)
  (if (null? fn_names) -1
      (if (= (car (car fn_names)) 'main)
          (cdr (car fn_names))
          (find_main_fn (cdr fn_names)))))

;; ============================================================
;; Serialization
;; ============================================================

(define (serialize_syms out syms idx max_idx)
  (if (< idx max_idx)
      (let name (sym_find_by_idx syms idx)
        (emit_u32 out (str.length name))
        (buf.push_string out name)
        (serialize_syms out syms (+ idx 1) max_idx))
      'nil))

(define (ser_fn_table out define_off lambda_off idx total_fns top_fn_id top_off)
  (if (< idx total_fns)
      (begin
        (if (= idx top_fn_id)
            (emit_u32 out top_off)
            (emit_u32 out (find_fn_entry define_off lambda_off idx)))
        (ser_fn_table out define_off lambda_off (+ idx 1) total_fns top_fn_id top_off))))

(define (copy_bytes dest src pos len)
  (if (< pos len)
      (begin
        (buf.push_byte dest (buf.get_byte src pos))
        (copy_bytes dest src (+ pos 1) len))
      'nil))

;; ============================================================
;; Top-level expression compilation
;; ============================================================

(define (compile_top_level forms b fn_names fn_entries next_fn_id)
  (if (null? forms) 0
      (if (pair? (car forms))
          (let head (car (car forms))
            (if (= (is_define_form head) 1)
                (compile_top_level (cdr forms) b fn_names fn_entries next_fn_id)
                (if (= head 'import)
                    (compile_top_level (cdr forms) b fn_names fn_entries next_fn_id)
                    (if (= head 'type)
                        (compile_top_level (cdr forms) b fn_names fn_entries next_fn_id)
                                                (begin
                          (compile_expr b (car forms) 0
                            (make_state fn_names next_fn_id fn_entries 'nil 0 0))
                          (buf.push_byte b 28)
                                    (compile_top_level (cdr forms) b fn_names fn_entries next_fn_id)))))))
          (compile_top_level (cdr forms) b fn_names fn_entries next_fn_id))))

;; ============================================================
;; Top-level compilation
;; ============================================================

(define (compile_code ast)
  (let b (buf.new)
    (let p1 (pass1_register ast 'nil 0)
      (let fn_names_base (car p1)
        (let n_funcs (cdr p1)
          (let sym_result (collect_ast_list ast (init_syms) 42)
            (let full_syms (car sym_result)
              (let fn_names (embed_syms fn_names_base full_syms)
          (buf.push_byte b 26)
          (let jump_pos (buf.length b)
            (emit_u32 b 0)
            (let p2 (pass2_compile ast b fn_names (+ n_funcs 1) 'nil 0 'nil)
              (let total_fns (car p2)
                (let fn_entries (car (cdr p2))
                  (let fn_offsets (cdr (cdr p2))
                    (let top_entry (buf.length b)
                      (patch_u32 b jump_pos top_entry)
                      (let has_top (compile_top_level ast b fn_names fn_entries total_fns)
                        (if (= has_top 0)
                            (let main_fid (find_main_fn fn_names)
                              (if (>= main_fid 0)
                                  (begin (buf.push_byte b 35) (emit_u32 b main_fid))
                                  'nil))
                            'nil)
                        (buf.push_byte b 0)
                        (buf.push_byte b 44)
                        (cons b (cons total_fns (cons n_funcs (cons top_entry
                                                                                          (cons fn_offsets (cons fn_entries full_syms)))))))))))))))))))))

;; ============================================================
;; Serialize to .tabc
;; ============================================================

(define (serialize_tabc info)
  (let b (car info)
    (let total_fns (car (cdr info))
            (let n_funcs (car (cdr (cdr info)))
        (let top_entry (car (cdr (cdr (cdr info))))
          (let fn_offsets (car (cdr (cdr (cdr (cdr info)))))
            (let fn_entries (car (cdr (cdr (cdr (cdr (cdr info))))))
              (let syms (cdr (cdr (cdr (cdr (cdr (cdr info))))))
                (let n_syms (sym_count syms)
                  (let out (buf.new)
                    (buf.push_byte out 84)
                    (buf.push_byte out 65)
                    (buf.push_byte out 66)
                    (buf.push_byte out 67)
                    (emit_u32 out 1)
                                        (emit_u32 out n_syms)
                    (emit_u32 out total_fns)
                    (emit_u32 out n_funcs)
                    (emit_u32 out (buf.length b))
                                        (serialize_syms out syms 0 n_syms)
                    (ser_fn_table out fn_offsets fn_entries 0 total_fns n_funcs top_entry)
                    (copy_bytes out b 0 (buf.length b))
                    out))))))))))

;; ============================================================
;; Public API
;; ============================================================

(define (compile ast)
  (let info (compile_code ast)
    (serialize_tabc info)))

(define (compile_and_write ast out_path)
  (let out (compile ast)
    (buf.write_to out out_path)))