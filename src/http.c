/*
 * http.c — HTTP helper module for TinyActor VM
 *
 * Provides:
 *   http.parse_request(raw)   → (method . path)  pair of strings
 *   http.response(status, content_type, body) → response string
 */

#include "ta.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
 * http.parse_request(data_string)
 *   Parse "GET /path HTTP/1.1\r\n..." into (method . path).
 *   Returns nil on parse failure.
 */
static Val http_parse_request(VM *vm, Val *args, int nargs) {
    (void)nargs;
    Proc *p = vm->current_proc;

    if (!val_is_string(args[0])) return val_nil();
    HeapString *hs = val_get_string(args[0]);
    const char *data = hs->data;
    int len = hs->len;

    /* Find end of request line (first \r\n) */
    const char *crlf = memmem(data, len, "\r\n", 2);
    if (!crlf) return val_nil();

    int line_len = (int)(crlf - data);

    /* Find first space: end of method */
    const char *sp1 = memchr(data, ' ', line_len);
    if (!sp1) return val_nil();

    int method_len = (int)(sp1 - data);
    const char *method_start = data;

    /* Find second space: end of path */
    const char *sp2 = memchr(sp1 + 1, ' ', line_len - method_len - 1);
    if (!sp2) return val_nil();

    int path_len = (int)(sp2 - sp1 - 1);
    const char *path_start = sp1 + 1;

    /* Build (method . path) pair */
    gc_root_push(p, args[0]);  /* protect input from GC */

    Val method = val_string(p, method_start, method_len);
    gc_root_push(p, method);

    Val path = val_string(p, path_start, path_len);
    gc_root_push(p, path);

    Val pair = val_pair(p, method, path);

    gc_root_pop(p); /* path */
    gc_root_pop(p); /* method */
    gc_root_pop(p); /* args[0] */

    return pair;
}

/*
 * http.response(status_code, content_type_string, body_string)
 *   Build "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 5\r\n\r\nHello"
 */
static Val http_response(VM *vm, Val *args, int nargs) {
    (void)nargs;
    Proc *p = vm->current_proc;

    int status = (int)val_get_int(args[0]);

    if (!val_is_string(args[1]) || !val_is_string(args[2])) return val_nil();

    HeapString *ct = val_get_string(args[1]);
    HeapString *body = val_get_string(args[2]);

    /* Status text */
    const char *status_text;
    switch (status) {
        case 200: status_text = "OK"; break;
        case 201: status_text = "Created"; break;
        case 204: status_text = "No Content"; break;
        case 301: status_text = "Moved Permanently"; break;
        case 302: status_text = "Found"; break;
        case 304: status_text = "Not Modified"; break;
        case 400: status_text = "Bad Request"; break;
        case 401: status_text = "Unauthorized"; break;
        case 403: status_text = "Forbidden"; break;
        case 404: status_text = "Not Found"; break;
        case 405: status_text = "Method Not Allowed"; break;
        case 500: status_text = "Internal Server Error"; break;
        case 502: status_text = "Bad Gateway"; break;
        case 503: status_text = "Service Unavailable"; break;
        default:  status_text = "Unknown"; break;
    }

    /* Protect args from GC */
    gc_root_push(p, args[0]);
    gc_root_push(p, args[1]);
    gc_root_push(p, args[2]);

    char header[1024];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %.*s\r\n"
        "Content-Length: %d\r\n"
        "\r\n",
        status, status_text,
        ct->len, ct->data,
        body->len);

    /* Allocate a single string: header + body */
    int total = header_len + body->len;
    char *buf = malloc(total);
    if (!buf) {
        gc_root_pop(p); gc_root_pop(p); gc_root_pop(p);
        return val_nil();
    }
    memcpy(buf, header, header_len);
    memcpy(buf + header_len, body->data, body->len);

    gc_root_push(p, args[0]); /* extra root for safety */

    Val result = val_string(p, buf, total);

    free(buf);

    gc_root_pop(p);
    gc_root_pop(p);
    gc_root_pop(p);
    gc_root_pop(p);

    return result;
}

static TaFunc http_funcs[] = {
    {"parse_request", http_parse_request, 1},
    {"response",      http_response,      3},
    {NULL, NULL, 0}
};

void vm_register_http_module(VM *vm) {
    vm_register_module(vm, "http", http_funcs, 2);
}