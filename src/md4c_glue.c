/*
 * md4c_glue.c — markdown SAX glue for the md stdlib module (static,
 * stdlib-port-plan Workstream B, extracted from go.blog src-ta/md4c).
 *
 * md4c (vendored verbatim in src/md4c/) is a SAX-style parser: this glue
 * translates its callbacks into a flat, document-ordered event list. It
 * builds no tree and no ADT — lib/md.ta folds the events into the Node
 * ADT (task-list items, ol start attr, code-block language attr).
 *
 *   md.raw_parse(text)  -> List(Event) | -1
 *
 * -1 is the atomic hard-error signal (docs/c-module.md): md_parse only
 * fails on out-of-memory (or a parser abort callback, which we do not
 * install), so a -1 means "out of memory". Markdown itself has no syntax
 * errors — everything parses to something. lib/md.ta lifts the -1 into
 * Err; malformed non-string arguments also return -1 (the typechecker
 * makes this unreachable from well-typed callers).
 *
 * Event shape: every event is a pair (sym . payload). The full grammar is
 * documented at the top of lib/md.ta; the two sides must stay in sync.
 *
 * Extraction deltas vs go.blog src-ta/md4c/glue.c:
 *   - parses an in-memory string (md.parse(text)) instead of a file path
 *     (go.blog's md4c.events(path) fopen'd + fread the file itself)
 *   - returns -1 instead of nil on failure (signal vocabulary: nil is the
 *     suspension signal in this repo, not an error)
 *   - static registration under the full dotted name "md.raw_parse"
 *     (src/encoding.c pattern), NOT a module-registry entry: a registry
 *     entry for "md" would make `import md` a compile-time no-op
 *     (is_builtin_module) and lib/md.ta — the facade that owns the public
 *     API and the Node fold — would never load. The raw_ prefix also
 *     matters the other way round: codegen prefers the cfunc table over
 *     TA functions for a dotted symbol, so a C name equal to a public TA
 *     name would silently bypass the lift.
 *   - parser configuration is unchanged and byte-compatible with the
 *     go.blog / cora wrap.c shape: MD_FLAG_STRIKETHROUGH |
 *     MD_FLAG_TASKLISTS; MD_TEXT_HTML and MD_BLOCK_DOC/HTML/TH/TD emit
 *     nothing; MD_TEXT_SOFTBR falls through to plain text (md4c delivers
 *     it as the 1-byte string "\n").
 *
 * GC discipline: none needed — a C module call is wrapped in the
 * proc_gc_enter gate (no collection can run until it returns) and the
 * actor arena never moves after first allocation (tinyactor issue #160).
 * val_pair roots car and cdr itself before its allocation, so a freshly
 * built payload is safe to pass directly.
 *
 * Known boundary (documented, deliberately not fixed): md4c_attr_text
 * returns nil on malloc failure instead of the -1 hard-error signal, so
 * an OOM while building an attr string silently drops that href/src/
 * title/lang. The window is tiny (attr text is far smaller than the
 * parse buffer md_parse already holds; if this malloc fails, md_parse
 * will OOM into -1 moments later) — per AGENTS.md the complexity of an
 * OOM flag through the callbacks is not worth it.
 */

#include "md4c/md4c.h"
#include "ta.h"

#include <stdio.h>
#include <string.h>

/* ---- pre-interned event symbols (set in vm_register_md_module) ---- */

static Val sym_e_p, sym_e_quote, sym_e_ul, sym_e_ol, sym_e_li, sym_e_h, sym_e_code, sym_e_hr,
    sym_e_table, sym_e_thead, sym_e_tbody, sym_e_tr;
static Val sym_l_p, sym_l_quote, sym_l_ul, sym_l_ol, sym_l_li, sym_l_h, sym_l_code, sym_l_table,
    sym_l_thead, sym_l_tbody, sym_l_tr;
static Val sym_s_em, sym_s_strong, sym_s_code, sym_s_del, sym_s_a, sym_s_img, sym_sl_em,
    sym_sl_strong, sym_sl_code, sym_sl_del, sym_sl_a, sym_sl_img;
static Val sym_text, sym_br;

/* ---- SAX state ---- */

typedef struct {
    Proc *p;
    Val acc; /* reversed event list accumulator */
} md4c_state;

/* Append one (sym . payload) event. */
static void md4c_event(md4c_state *s, Val sym, Val payload) {
    Val ev = val_pair(s->p, sym, payload); /* ev fresh; alloc rooted its args */
    s->acc = val_pair(s->p, ev, s->acc);
}

static void md4c_event_nil(md4c_state *s, Val sym) { md4c_event(s, sym, val_nil()); }

/* Concatenate all substrings of an MD_ATTRIBUTE (href/src/title/lang)
 * into one heap string. */
static Val md4c_attr_text(md4c_state *s, const MD_ATTRIBUTE *attr) {
    if (attr->text == NULL)
        return val_nil();
    MD_SIZE total = attr->size;
    if (total == 0)
        return val_string(s->p, "", 0);
    char *buf = malloc(total);
    if (!buf)
        return val_nil();
    MD_SIZE pos = 0;
    for (int i = 0; attr->substr_offsets[i] < attr->size; i++) {
        MD_SIZE off = attr->substr_offsets[i];
        MD_SIZE len = attr->substr_offsets[i + 1] - off;
        memcpy(buf + pos, attr->text + off, len);
        pos += len;
    }
    Val v = val_string(s->p, buf, (int)total);
    free(buf);
    return v;
}

/* ---- block callbacks ---- */

static int enter_block(MD_BLOCKTYPE type, void *detail, void *userdata) {
    md4c_state *s = (md4c_state *)userdata;
    switch (type) {
    case MD_BLOCK_DOC:
    case MD_BLOCK_HTML:
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        /* DOC/HTML are noops; TH/TD have no enter case (cells are plain
         * text carriers — no blog table needs cell markup). */
        break;
    case MD_BLOCK_QUOTE:
        md4c_event_nil(s, sym_e_quote);
        break;
    case MD_BLOCK_UL:
        md4c_event_nil(s, sym_e_ul);
        break;
    case MD_BLOCK_OL: {
        const MD_BLOCK_OL_DETAIL *det = (const MD_BLOCK_OL_DETAIL *)detail;
        /* payload = start; the fold emits a start attribute only when
         * start != 1 — including start == 0 (markdown "0. item" renders
         * start="0"). */
        md4c_event(s, sym_e_ol, val_int((int)det->start));
        break;
    }
    case MD_BLOCK_LI: {
        const MD_BLOCK_LI_DETAIL *det = (const MD_BLOCK_LI_DETAIL *)detail;
        /* payload 0 = plain <li>; otherwise the task mark char ('x'/'X'
         * becomes a checked checkbox at fold time). */
        int mark = det->is_task ? (int)det->task_mark : 0;
        md4c_event(s, sym_e_li, val_int(mark));
        break;
    }
    case MD_BLOCK_HR:
        md4c_event_nil(s, sym_e_hr);
        break;
    case MD_BLOCK_H: {
        const MD_BLOCK_H_DETAIL *det = (const MD_BLOCK_H_DETAIL *)detail;
        md4c_event(s, sym_e_h, val_int(det->level));
        break;
    }
    case MD_BLOCK_CODE: {
        const MD_BLOCK_CODE_DETAIL *det = (const MD_BLOCK_CODE_DETAIL *)detail;
        /* lang string, or nil (info attr only when lang.text != NULL). */
        md4c_event(s, sym_e_code, md4c_attr_text(s, &det->lang));
        break;
    }
    case MD_BLOCK_P:
        md4c_event_nil(s, sym_e_p);
        break;
    case MD_BLOCK_TABLE:
        md4c_event_nil(s, sym_e_table);
        break;
    case MD_BLOCK_THEAD:
        md4c_event_nil(s, sym_e_thead);
        break;
    case MD_BLOCK_TBODY:
        md4c_event_nil(s, sym_e_tbody);
        break;
    case MD_BLOCK_TR:
        md4c_event_nil(s, sym_e_tr);
        break;
    default:
        break;
    }
    return 0;
}

static int leave_block(MD_BLOCKTYPE type, void *detail, void *userdata) {
    md4c_state *s = (md4c_state *)userdata;
    (void)detail;
    switch (type) {
    case MD_BLOCK_DOC:
    case MD_BLOCK_HR:
    case MD_BLOCK_HTML:
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        break;
    case MD_BLOCK_QUOTE:
        md4c_event_nil(s, sym_l_quote);
        break;
    case MD_BLOCK_UL:
        md4c_event_nil(s, sym_l_ul);
        break;
    case MD_BLOCK_OL:
        md4c_event_nil(s, sym_l_ol);
        break;
    case MD_BLOCK_LI:
        md4c_event_nil(s, sym_l_li);
        break;
    case MD_BLOCK_H:
        md4c_event_nil(s, sym_l_h);
        break;
    case MD_BLOCK_CODE:
        /* one leave event; the fold closes both <code> and <pre>. */
        md4c_event_nil(s, sym_l_code);
        break;
    case MD_BLOCK_P:
        md4c_event_nil(s, sym_l_p);
        break;
    case MD_BLOCK_TABLE:
        md4c_event_nil(s, sym_l_table);
        break;
    case MD_BLOCK_THEAD:
        md4c_event_nil(s, sym_l_thead);
        break;
    case MD_BLOCK_TBODY:
        md4c_event_nil(s, sym_l_tbody);
        break;
    case MD_BLOCK_TR:
        md4c_event_nil(s, sym_l_tr);
        break;
    default:
        break;
    }
    return 0;
}

/* ---- span callbacks ---- */

static int enter_span(MD_SPANTYPE type, void *detail, void *userdata) {
    md4c_state *s = (md4c_state *)userdata;
    switch (type) {
    case MD_SPAN_EM:
        md4c_event_nil(s, sym_s_em);
        break;
    case MD_SPAN_STRONG:
        md4c_event_nil(s, sym_s_strong);
        break;
    case MD_SPAN_CODE:
        md4c_event_nil(s, sym_s_code);
        break;
    case MD_SPAN_DEL:
        md4c_event_nil(s, sym_s_del);
        break;
    case MD_SPAN_A: {
        const MD_SPAN_A_DETAIL *det = (const MD_SPAN_A_DETAIL *)detail;
        /* payload: (href . title); title is nil when absent. Plain C
         * locals are safe across allocations — no GC can run inside a
         * C module call (issue #160 gate model). */
        Val href = md4c_attr_text(s, &det->href);
        Val title = val_nil();
        if (det->title.text != NULL)
            title = md4c_attr_text(s, &det->title);
        Val payload = val_pair(s->p, href, title);
        md4c_event(s, sym_s_a, payload);
        break;
    }
    case MD_SPAN_IMG: {
        const MD_SPAN_IMG_DETAIL *det = (const MD_SPAN_IMG_DETAIL *)detail;
        /* payload: src string (title attr dropped; alt text arrives via
         * text callbacks between the img enter/leave). */
        md4c_event(s, sym_s_img, md4c_attr_text(s, &det->src));
        break;
    }
    default:
        /* U / LATEXMATH / WIKILINK: parser flags never enable them. */
        break;
    }
    return 0;
}

static int leave_span(MD_SPANTYPE type, void *detail, void *userdata) {
    md4c_state *s = (md4c_state *)userdata;
    (void)detail;
    switch (type) {
    case MD_SPAN_EM:
        md4c_event_nil(s, sym_sl_em);
        break;
    case MD_SPAN_STRONG:
        md4c_event_nil(s, sym_sl_strong);
        break;
    case MD_SPAN_CODE:
        md4c_event_nil(s, sym_sl_code);
        break;
    case MD_SPAN_DEL:
        md4c_event_nil(s, sym_sl_del);
        break;
    case MD_SPAN_A:
        md4c_event_nil(s, sym_sl_a);
        break;
    case MD_SPAN_IMG:
        md4c_event_nil(s, sym_sl_img);
        break;
    default:
        break;
    }
    return 0;
}

/* ---- text callback ---- */

static int text_cb(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *userdata) {
    md4c_state *s = (md4c_state *)userdata;
    switch (type) {
    case MD_TEXT_BR:
        md4c_event_nil(s, sym_br);
        break;
    case MD_TEXT_HTML:
        /* dropped: raw HTML passes through nowhere (same as the cora
         * wrap.c this extraction is verified against). */
        break;
    default:
        /* NORMAL, ENTITY, CODE, ESCAPED, SOFTBR, ... : raw text passed
         * through as-is — a soft break reaches this default and md4c
         * delivers it as the 1-byte text "\n", so soft line breaks
         * survive as text nodes. */
        md4c_event(s, sym_text, val_string(s->p, (const char *)text, (int)size));
        break;
    }
    return 0;
}

static void debug_log(const char *msg, void *userdata) {
    (void)msg;
    (void)userdata;
}

/* ---- module entry ---- */

static Val md_raw_parse(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    Proc *p = tls_current_proc;
    if (!p || !val_is_string(args[0]))
        return val_int(-1);
    HeapString *hs = val_get_string(args[0]);

    md4c_state s;
    s.p = p;
    s.acc = val_nil();

    static const unsigned parser_flags = MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS;

    MD_PARSER parser = {0,          parser_flags, enter_block, leave_block, enter_span,
                        leave_span, text_cb,      debug_log,   NULL};

    /* No rooting needed: no GC can run inside a C module call (the
     * proc_gc_enter gate defers collection until after we return), and
     * the arena never moves — the accumulator is safe in a C struct. */
    int ret = md_parse(hs->data, (MD_SIZE)hs->len, &parser, &s);
    if (ret != 0)
        return val_int(-1); /* out of memory only (see header) */

    /* Reverse the accumulator back to document order (event level only). */
    Val rev = val_nil();
    Val it = s.acc;
    while (!val_is_nil(it)) {
        rev = val_pair(p, val_get_car(it), rev);
        it = val_get_cdr(it);
    }
    return rev;
}

static TaFunc md_funcs[] = {{"raw_parse", md_raw_parse, 1}, {NULL, NULL, 0}};

/* encoding.c registration pattern: full dotted name, NO module-registry
 * entry — see the header comment for why ("import md" must load
 * lib/md.ta). */
void vm_register_md_module(VM *vm) {
    /* Pre-intern all event symbols up front (avoid runtime interning). */
    sym_e_p = val_symbol(vm_intern_symbol(vm, "e-p"));
    sym_e_quote = val_symbol(vm_intern_symbol(vm, "e-quote"));
    sym_e_ul = val_symbol(vm_intern_symbol(vm, "e-ul"));
    sym_e_ol = val_symbol(vm_intern_symbol(vm, "e-ol"));
    sym_e_li = val_symbol(vm_intern_symbol(vm, "e-li"));
    sym_e_h = val_symbol(vm_intern_symbol(vm, "e-h"));
    sym_e_code = val_symbol(vm_intern_symbol(vm, "e-code"));
    sym_e_hr = val_symbol(vm_intern_symbol(vm, "e-hr"));
    sym_e_table = val_symbol(vm_intern_symbol(vm, "e-table"));
    sym_e_thead = val_symbol(vm_intern_symbol(vm, "e-thead"));
    sym_e_tbody = val_symbol(vm_intern_symbol(vm, "e-tbody"));
    sym_e_tr = val_symbol(vm_intern_symbol(vm, "e-tr"));
    sym_l_p = val_symbol(vm_intern_symbol(vm, "l-p"));
    sym_l_quote = val_symbol(vm_intern_symbol(vm, "l-quote"));
    sym_l_ul = val_symbol(vm_intern_symbol(vm, "l-ul"));
    sym_l_ol = val_symbol(vm_intern_symbol(vm, "l-ol"));
    sym_l_li = val_symbol(vm_intern_symbol(vm, "l-li"));
    sym_l_h = val_symbol(vm_intern_symbol(vm, "l-h"));
    sym_l_code = val_symbol(vm_intern_symbol(vm, "l-code"));
    sym_l_table = val_symbol(vm_intern_symbol(vm, "l-table"));
    sym_l_thead = val_symbol(vm_intern_symbol(vm, "l-thead"));
    sym_l_tbody = val_symbol(vm_intern_symbol(vm, "l-tbody"));
    sym_l_tr = val_symbol(vm_intern_symbol(vm, "l-tr"));
    sym_s_em = val_symbol(vm_intern_symbol(vm, "s-em"));
    sym_s_strong = val_symbol(vm_intern_symbol(vm, "s-strong"));
    sym_s_code = val_symbol(vm_intern_symbol(vm, "s-code"));
    sym_s_del = val_symbol(vm_intern_symbol(vm, "s-del"));
    sym_s_a = val_symbol(vm_intern_symbol(vm, "s-a"));
    sym_s_img = val_symbol(vm_intern_symbol(vm, "s-img"));
    sym_sl_em = val_symbol(vm_intern_symbol(vm, "sl-em"));
    sym_sl_strong = val_symbol(vm_intern_symbol(vm, "sl-strong"));
    sym_sl_code = val_symbol(vm_intern_symbol(vm, "sl-code"));
    sym_sl_del = val_symbol(vm_intern_symbol(vm, "sl-del"));
    sym_sl_a = val_symbol(vm_intern_symbol(vm, "sl-a"));
    sym_sl_img = val_symbol(vm_intern_symbol(vm, "sl-img"));
    sym_text = val_symbol(vm_intern_symbol(vm, "text"));
    sym_br = val_symbol(vm_intern_symbol(vm, "br"));

    for (int i = 0; md_funcs[i].name != NULL; i++) {
        char qualified[64];
        snprintf(qualified, sizeof(qualified), "md.%s", md_funcs[i].name);
        vm_register(vm, qualified, md_funcs[i].fn, md_funcs[i].nargs);
    }
}