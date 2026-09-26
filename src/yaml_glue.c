/*
 * yaml_glue.c — libyaml SAX glue for the yaml stdlib module (static,
 * stdlib-port-plan Phase 7 #2, vendored-libyaml shape of md4c_glue.c).
 *
 * libyaml (vendored verbatim in src/yaml/, parser-only subset) is an
 * event-stream parser: this glue translates its events into a flat,
 * document-ordered event list. It builds no tree and no ADT — lib/yaml.ta
 * folds the events into the Yaml Value ADT (json-shaped) and owns all
 * scalar resolution (plain-scalar int/float/bool/null classification).
 *
 *   yaml.raw_parse(text) -> (ok . events)
 *                         | (err . (msg . (line . col)))
 *                         | -1
 *
 * -1 is the atomic hard-error signal (docs/c-module.md): out of memory
 * or a malformed non-string argument (the typechecker makes the latter
 * unreachable from well-typed callers). Syntax errors — where libyaml's
 * error channel carries a message and a problem mark — are lifted into
 * the (err . ...) shape with 1-based line/col; lib/yaml.ta formats them
 * "line L, col C: msg" (same shape as mpc.fmt_err).
 *
 * Event shape: every event is a pair (sym . payload). The full grammar
 * is documented at the top of lib/yaml.ta; the two sides must stay in
 * sync. anchor payloads are a string or nil; plain is 1/0:
 *
 *   (map-s . anchor) (map-e . nil) (seq-s . anchor) (seq-e . nil)
 *   (scalar . (text . (plain . anchor)))
 *   (alias . name)
 *
 * Stream/document bookkeeping the fold never sees:
 *   - DOCUMENT_START on a second document in one stream -> (err ...) —
 *     the Value ADT is single-document (json-shaped), so a multi-doc
 *     stream is refused, not silently truncated.
 *   - a stream that ends with no node event at all (empty input, or
 *     comments/whitespace only) -> (err ... "empty document").
 *
 * Dropped by policy (documented in lib/yaml.ta): tags (!!str etc. — the
 * node's own style decides resolution), version directives, document
 * markers. libyaml already folds block scalars, multi-line flow
 * collections and escapes into the delivered scalar text.
 *
 * GC discipline: none needed — a C module call is wrapped in the
 * proc_gc_enter gate (no collection can run until it returns) and the
 * actor arena never moves after first allocation (tinyactor issue #160).
 * val_pair roots car and cdr itself before its allocation, so a freshly
 * built payload is safe to pass directly.
 *
 * Static registration under the full dotted name "yaml.raw_parse"
 * (src/md4c_glue.c / src/encoding.c pattern), NOT a module-registry
 * entry: a registry entry for "yaml" would make `import yaml` a
 * compile-time no-op (is_builtin_module) and lib/yaml.ta — the facade
 * that owns the public API and the fold — would never load.
 */

#include "ta.h"
#include "yaml/yaml.h"

#include <stdio.h>
#include <string.h>

/* ---- pre-interned event symbols (set in vm_register_yaml_module) ---- */

static Val sym_ok, sym_err;
static Val sym_map_s, sym_map_e, sym_seq_s, sym_seq_e, sym_scalar, sym_alias;

/* ---- SAX state ---- */

typedef struct {
    Proc *p;
    Val acc; /* reversed event list accumulator */
} yaml_state;

/* Append one (sym . payload) event. */
static void yaml_event_push(yaml_state *s, Val sym, Val payload) {
    Val ev = val_pair(s->p, sym, payload); /* ev fresh; alloc rooted its args */
    s->acc = val_pair(s->p, ev, s->acc);
}

/* anchor (yaml_char_t*, NULL when absent) -> string Val or nil. */
static Val yaml_anchor_val(Proc *p, const unsigned char *a) {
    if (a == NULL)
        return val_nil();
    return val_string(p, (const char *)a, (int)strlen((const char *)a));
}

/* (err . (msg . (line . col))) from a yaml_mark_t (0-based -> 1-based). */
static Val yaml_err_mark(Proc *p, const char *msg, yaml_mark_t mark) {
    Val payload = val_pair(p, val_string(p, msg, (int)strlen(msg)),
                           val_pair(p, val_int((int)mark.line + 1), val_int((int)mark.column + 1)));
    return val_pair(p, sym_err, payload);
}

/* (err . ...) from the parser's error channel (message + problem mark). */
static Val yaml_err_parser(Proc *p, yaml_parser_t *parser) {
    const char *msg = parser->problem ? parser->problem : "parse error";
    return yaml_err_mark(p, msg, parser->problem_mark);
}

/* ---- module entry ---- */

static Val yaml_raw_parse(VM *vm, Val *args, int nargs) {
    (void)vm;
    (void)nargs;
    Proc *p = tls_current_proc;
    if (!p || !val_is_string(args[0]))
        return val_int(-1);
    HeapString *hs = val_get_string(args[0]);

    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser))
        return val_int(-1); /* out of memory only */
    yaml_parser_set_input_string(&parser, (const unsigned char *)hs->data, (size_t)hs->len);

    yaml_state s;
    s.p = p;
    s.acc = val_nil();

    int docs = 0;                     /* DOCUMENT_START count — single-document contract */
    int node_events = 0;              /* scalar/map/seq count — empty-stream detection */
    yaml_mark_t end_mark = {0, 0, 0}; /* STREAM_END mark (empty-stream error) */
    Val result = val_nil();
    int failed = 0;

    /* No rooting needed: no GC can run inside a C module call (the
     * proc_gc_enter gate defers collection until after we return), and
     * the arena never moves — the accumulator is safe in a C struct. */
    yaml_event_t ev;
    int stop = 0;
    while (!stop) {
        if (!yaml_parser_parse(&parser, &ev)) {
            result = yaml_err_parser(p, &parser);
            failed = 1;
            break;
        }
        switch (ev.type) {
        case YAML_STREAM_END_EVENT:
            stop = 1;
            break;
        case YAML_DOCUMENT_START_EVENT:
            docs++;
            if (docs > 1) {
                result = yaml_err_mark(p, "multiple YAML documents in one stream are not supported",
                                       ev.start_mark);
                failed = 1;
                stop = 1;
            }
            break;
        case YAML_DOCUMENT_END_EVENT:
        case YAML_STREAM_START_EVENT:
            break;
        case YAML_MAPPING_START_EVENT:
            node_events++;
            yaml_event_push(&s, sym_map_s, yaml_anchor_val(p, ev.data.mapping_start.anchor));
            break;
        case YAML_MAPPING_END_EVENT:
            yaml_event_push(&s, sym_map_e, val_nil());
            break;
        case YAML_SEQUENCE_START_EVENT:
            node_events++;
            yaml_event_push(&s, sym_seq_s, yaml_anchor_val(p, ev.data.sequence_start.anchor));
            break;
        case YAML_SEQUENCE_END_EVENT:
            yaml_event_push(&s, sym_seq_e, val_nil());
            break;
        case YAML_SCALAR_EVENT: {
            node_events++;
            /* payload: (text . (plain . anchor)); plain = 1 only for
             * YAML_PLAIN_SCALAR_STYLE (the resolution-relevant bit). */
            Val text =
                val_string(p, (const char *)ev.data.scalar.value, (int)ev.data.scalar.length);
            Val plain = val_int(ev.data.scalar.style == YAML_PLAIN_SCALAR_STYLE ? 1 : 0);
            Val anchor = yaml_anchor_val(p, ev.data.scalar.anchor);
            Val payload = val_pair(p, text, val_pair(p, plain, anchor));
            yaml_event_push(&s, sym_scalar, payload);
            break;
        }
        case YAML_ALIAS_EVENT:
            node_events++;
            yaml_event_push(&s, sym_alias, yaml_anchor_val(p, ev.data.alias.anchor));
            break;
        default:
            break;
        }
        yaml_event_delete(&ev);
    }
    yaml_parser_delete(&parser);
    if (failed)
        return result;

    if (node_events == 0)
        return yaml_err_mark(p, "empty document", end_mark);

    /* Reverse the accumulator back to document order (event level only). */
    Val rev = val_nil();
    Val it = s.acc;
    while (!val_is_nil(it)) {
        rev = val_pair(p, val_get_car(it), rev);
        it = val_get_cdr(it);
    }
    return val_pair(p, sym_ok, rev);
}

static TaFunc yaml_funcs[] = {{"raw_parse", yaml_raw_parse, 1}, {NULL, NULL, 0}};

/* md4c_glue.c registration pattern: full dotted name, NO module-registry
 * entry — see the header comment for why ("import yaml" must load
 * lib/yaml.ta). */
void vm_register_yaml_module(VM *vm) {
    /* Pre-intern all symbols up front (avoid runtime interning). */
    sym_ok = val_symbol(vm_intern_symbol(vm, "ok"));
    sym_err = val_symbol(vm_intern_symbol(vm, "err"));
    sym_map_s = val_symbol(vm_intern_symbol(vm, "map-s"));
    sym_map_e = val_symbol(vm_intern_symbol(vm, "map-e"));
    sym_seq_s = val_symbol(vm_intern_symbol(vm, "seq-s"));
    sym_seq_e = val_symbol(vm_intern_symbol(vm, "seq-e"));
    sym_scalar = val_symbol(vm_intern_symbol(vm, "scalar"));
    sym_alias = val_symbol(vm_intern_symbol(vm, "alias"));

    for (int i = 0; yaml_funcs[i].name != NULL; i++) {
        char qualified[64];
        snprintf(qualified, sizeof(qualified), "yaml.%s", yaml_funcs[i].name);
        vm_register(vm, qualified, yaml_funcs[i].fn, yaml_funcs[i].nargs);
    }
}