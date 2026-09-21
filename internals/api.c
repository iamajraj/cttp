/* ==========================================================================
 * api.c — the helper layer handlers call. No snprintf required anywhere.
 *
 * Three groups live here:
 *   1. request accessors (param / query / form / cookie / request id)
 *   2. response builders (text/json/redirect/cookies/cors/...)
 *   3. JSON builder + SSE push streaming + small utilities
 * ========================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <time.h>

#include "cttp.h"

/* ==== request accessors ==================================================== */

/* Value captured for a ":name" parameter of the matched route. */
const char *cttp_param(const cttp_request *r, const char *name)
{
    for (int i = 0; i < r->nparams; i++)
        if (strcmp(r->params[i], name) == 0)
            return r->pvalues[i];
    return NULL;
}

/* Split the next "a=b" pair of a query/form/cookie string into decoded
 * key and value. Returns 1 on read, 0 when the string is exhausted.
 * Sections are separated by '&' (or ';' for cookies, chosen by caller
 * passing sep). '+' => space only when `plus` is set (query & forms; a
 * real space in a cookie value would appear as %20 instead).            */
static int next_pair(const char **cursor, char sep1, char sep2,
                     char *key, size_t kn, char *val, size_t vn, int plus)
{
    const char *p = *cursor;
    while (p && (*p == sep1 || *p == sep2)) p++;      /* skip separators */
    if (!p || !*p) { *cursor = p; return 0; }

    const char *end = p;
    while (*end && *end != sep1 && *end != sep2) end++;

    const char *eq = memchr(p, '=', (size_t)(end - p));
    if (eq) {
        char keyraw[128];
        size_t klen = (size_t)(eq - p);
        if (klen >= kn) klen = kn - 1;
        memcpy(keyraw, p, klen);
        keyraw[klen] = '\0';
        cttp_url_decode(key, kn, keyraw, plus);

        char valraw[512];
        size_t vlen = (size_t)(end - (eq + 1));
        if (vlen >= vn) vlen = vn - 1;
        memcpy(valraw, eq + 1, vlen);
        valraw[vlen] = '\0';
        cttp_url_decode(val, vn, valraw, plus);
    } else {
        snprintf(key, kn, "%.*s", (int)(end - p), p);
        cttp_url_decode(key, kn, key, plus);
        cttp_trim(key);
        val[0] = '\0';
    }

    *cursor = end;
    return 1;
}

/* Query strings: "?name=Ada%20L&q=1". We scan on demand and decode into
 * one of two rotating scratch slots (headers are read-only structs). */
const char *cttp_query(const cttp_request *r, const char *key)
{
    static char out[2][256];
    static int slot;
    slot ^= 1;
    const char *cur = r->query;
    char k[64], v[256];
    while (next_pair(&cur, '&', '&', k, sizeof k, v, sizeof v, 1))
        if (strcmp(k, key) == 0)
            return (snprintf(out[slot], sizeof out[slot], "%s", v),
                    out[slot]);
    return NULL;
}

int cttp_query_has(const cttp_request *r, const char *key)
{
    return cttp_query(r, key) != NULL;
}

/* Form bodies: "Content-Type: application/x-www-form-urlencoded" with
 * the same a=b&c=d shape as a query string, arriving as the body. */
const char *cttp_form(const cttp_request *r, const char *key)
{
    static char out[2][512];
    static int slot;
    if (!r->body.data) return NULL;
    const char *ct = cttp_header(r, "Content-Type");
    if (!ct || !cttp_streq_i(ct, "application/x-www-form-urlencoded"))
        return NULL;
    slot ^= 1;
    const char *cur = r->body.data;
    char k[64], v[256];
    while (next_pair(&cur, '&', '&', k, sizeof k, v, sizeof v, 1))
        if (strcmp(k, key) == 0)
            return (snprintf(out[slot], sizeof out[slot], "%s", v),
                    out[slot]);
    return NULL;
}

/* Cookies: "Cookie: a=1; b=2" — parsed once per request, then cached. */
const char *cttp_cookie(const cttp_request *r, const char *name)
{
    /* NOTE: r is const, so parse caches live in the request struct cast
     * away — same trick the router uses for params. */
    cttp_request *rw = (cttp_request *)r;
    if (rw->nccookies < 0) {
        const char *h = cttp_header(r, "Cookie");
        rw->nccookies = 0;
        const char *cur = h ? h : "";
        char k[64], v[256];
        while (rw->nccookies < 16 &&
               next_pair(&cur, ';', ';', k, sizeof k, v, sizeof v, 0)) {
            snprintf(rw->cookies[rw->nccookies].name,  64, "%s", k);
            snprintf(rw->cookies[rw->nccookies].value, 256, "%s", v);
            rw->nccookies++;
        }
    }
    for (int i = 0; i < rw->nccookies; i++)
        if (strcmp(rw->cookies[i].name, name) == 0)
            return rw->cookies[i].value;
    return NULL;
}

const char *cttp_req_id(const cttp_request *r) { return r->req_id; }

/* ==== small string utilities (url-decode lives in http.c) ==== */
/* Case-insensitive string equality (headers live up to RFC 9110 §5.1). */
int cttp_streq_i(const char *a, const char *b)
{
    return a && b && strcasecmp(a, b) == 0;
}

/* Trim ASCII whitespace in place; returns the (possibly shifted) start. */
const char *cttp_trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    char *e = p + strlen(p);
    while (e > p && (e[-1]==' '||e[-1]=='\t'||e[-1]=='\r'||e[-1]=='\n')) e--;
    *e = '\0';
    return p;
}

/* default case: defined with the JSON builder section, appended here */

/* ==== response builders ==================================================== */

/* anything filling in a response marks it: middleware stops right here */
static void responded(cttp_response *r) { r->responded = 1; }

void cttp_send(cttp_response *r, int status, const char *ctype,
               const void *body, size_t len)
{
    r->status = status;
    snprintf(r->ctype, sizeof r->ctype, "%s", ctype ? ctype : "text/plain");
    buf_free(&r->body);
    buf_append(&r->body, body, len);
    responded(r);
}

/* printf-style: never a fixed "snprintf then pray" buffer again */
void cttp_text(cttp_response *r, int status, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    r->status = status;
    snprintf(r->ctype, sizeof r->ctype, "text/plain; charset=utf-8");
    buf_free(&r->body);
    buf_vappendf(&r->body, fmt, ap);
    va_end(ap);
    responded(r);
}

void cttp_html(cttp_response *r, int status, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    r->status = status;
    snprintf(r->ctype, sizeof r->ctype, "text/html; charset=utf-8");
    buf_free(&r->body);
    buf_vappendf(&r->body, fmt, ap);
    va_end(ap);
    responded(r);
}

void cttp_no_content(cttp_response *r)
{
    r->status = 204;
    r->no_body = 1;               /* 204 must never carry a body */
    responded(r);
}

void cttp_redirect(cttp_response *r, int code, const char *location)
{
    r->status = code;             /* 301/302/307/308 */
    buf_free(&r->body);
    cttp_set_header(r, "Location", location);
    responded(r);
}

/* Header names are case-insensitive on the wire but must stay token-like:
 * letters, digits, '-', '_' — anything else is silently dropped so a
 * handler mistake can't smuggle CRLF into the response.                  */
static int valid_hdr_name(const char *n)
{
    for (const char *p = n; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
            return 0;
    return *n != '\0';
}

void cttp_set_header(cttp_response *r, const char *name, const char *value)
{
    if (!valid_hdr_name(name) || strchr(value, '\r') || strchr(value, '\n'))
        return;                   /* refuse CRLF injection via values    */
    buf_printf(&r->headers, "%s: %s\r\n", name, value);
}

void cttp_set_cookie(cttp_response *r, const char *name, const char *value,
                     const cttp_cookie_opts *opts)
{
    cttp_cookie_opts def = {0};
    if (!opts) opts = &def;
    buf_printf(&r->headers, "Set-Cookie: %s=%s", name, value);
    buf_printf(&r->headers, "; Path=%s",
               opts->path ? opts->path : "/");
    if (opts->max_age > 0)
        buf_printf(&r->headers, "; Max-Age=%d", opts->max_age);
    if (opts->domain)
        buf_printf(&r->headers, "; Domain=%s", opts->domain);
    if (opts->same_site)
        buf_printf(&r->headers, "; SameSite=%s", opts->same_site);
    if (opts->http_only)
        buf_append_str(&r->headers, "; HttpOnly");
    if (opts->secure)
        buf_append_str(&r->headers, "; Secure");
    buf_append_str(&r->headers, "\r\n");
}

void cttp_delete_cookie(cttp_response *r, const char *name)
{
    buf_printf(&r->headers,
               "Set-Cookie: %s=; Path=/; Max-Age=0\r\n", name);
}

void cttp_etag(cttp_response *r, const char *tag)
{
    buf_printf(&r->headers, "ETag: \"%s\"\r\n", tag);
}

void cttp_cache(cttp_response *r, int max_age_secs)
{
    buf_printf(&r->headers, "Cache-Control: max-age=%d\r\n", max_age_secs);
}

/* CORS: browsers force this dance for cross-origin JS calls.           */
void cttp_cors(cttp_response *r, const char *origin,
               const char *methods, const char *headers)
{
    cttp_set_header(r, "Access-Control-Allow-Origin", origin ? origin : "*");
    if (methods)
        cttp_set_header(r, "Access-Control-Allow-Methods", methods);
    else
        cttp_set_header(r, "Access-Control-Allow-Methods",
                        "GET, HEAD, POST, PUT, DELETE, OPTIONS");
    if (headers)
        cttp_set_header(r, "Access-Control-Allow-Headers", headers);
}

/* Suggest the client download instead of display. */
void cttp_attachment(cttp_response *r, const char *filename)
{
    char safe[256];
    snprintf(safe, sizeof safe, "%s", filename ? filename : "download");
    for (char *p = safe; *p; p++)          /* strip quotes/backslashes */
        if (*p == '"' || *p == '\\') *p = '_';
    char v[300];
    snprintf(v, sizeof v, "attachment; filename=\"%s\"", safe);
    cttp_set_header(r, "Content-Disposition", v);
}

/* ==== SSE push streaming ==================================================== */

/* Engine hook: wrap data into a wire chunk and queue it for the socket.
 * (conn is the engine's per-connection state, delivered via stream_handle.) */
static void stream_write(cttp_response *res, const void *data, size_t len)
{
    conn *c = res->stream_handle;
    if (!c) return;
    buf_printf(&c->out, "%zx\r\n", len);
    buf_append(&c->out, data, len);
    buf_append_str(&c->out, "\r\n");
    c->state = CONN_WRITE;               /* the loop flushes on POLLOUT */
}

/* Begin an SSE response: text/event-stream headers immediately, then
 * each cttp_sse_send call becomes its own delivered chunk.             */
void cttp_sse_start(cttp_response *res)
{
    conn *c = res->stream_handle;
    if (!c || c->streamed) return;
    c->streamed = 1;
    c->keep_alive = (c->req.version_minor >= 1);

    res->status = 200;
    res->stream = 1;
    res->responded = 1;
    snprintf(res->ctype, sizeof res->ctype, "text/event-stream");
    char date[64];
    cttp_http_date(date, sizeof date, time(NULL));
    buf_printf(&c->out,
        "HTTP/1.1 200 OK\r\nServer: cttp/%s\r\n"
        "Server-Request-Id: %s\r\nDate: %.64s\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\nTransfer-Encoding: chunked\r\n",
        CTTP_VERSION, c->req.req_id, date);
    buf_append(&c->out, res->headers.data, res->headers.len);
    buf_append_str(&c->out, "\r\n");

    c->state = CONN_WRITE;               /* the loop flushes on POLLOUT */
}

/* One event: "event: name\ndata: line\nodata...\n\n". Multi-line data
 * becomes several data: lines, exactly as the SSE spec wants.          */
void cttp_sse_send(cttp_response *res, const char *event, const char *data)
{
    if (!res->stream)
        cttp_sse_start(res);             /* implicit start if not called */

    buf_t ev = {0};
    if (event && *event)
        buf_printf(&ev, "event: %s\n", event);
    const char *p = data ? data : "";
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        buf_printf(&ev, "data: %.*s\n", (int)len, p);
        p = nl ? nl + 1 : p + len;
    }
    buf_append_str(&ev, "\n");
    stream_write(res, ev.data, ev.len);
    buf_free(&ev);
}

/* ==== JSON builder ========================================================== */

/* JSON string with full escaping — never trust raw concatenation.      */
static void json_quote(buf_t *b, const char *s)
{
    buf_append_str(b, "\"");
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"':  buf_append_str(b, "\\\""); break;
        case '\\': buf_append_str(b, "\\\\"); break;
        case '\n': buf_append_str(b, "\\n");  break;
        case '\t': buf_append_str(b, "\\t");  break;
        case '\r': buf_append_str(b, "\\r");  break;
        case '\b': buf_append_str(b, "\\b");  break;
        case '\f': buf_append_str(b, "\\f");  break;
        default:
            if (*p < 0x20)
                buf_printf(b, "\\u%04x", *p);
            else
                buf_append(b, p, 1);
        }
    }
    buf_append_str(b, "\"");
}

/* comma bookkeeping per nesting level */
static void json_key(cttp_response *r, const char *key)
{
    if (!r->_jf[r->_jd])
        buf_append_str(&r->body, ",");
    r->_jf[r->_jd] = 0;
    if (key)
        json_quote(&r->body, key), buf_append_str(&r->body, ":");
}

void cttp_json_begin(cttp_response *r)
{
    buf_free(&r->body);
    snprintf(r->ctype, sizeof r->ctype, "application/json");
    r->_jd = 0;
    r->_jf[0] = 1;
    buf_append_str(&r->body, "{");
}

void cttp_json_str(cttp_response *r, const char *key, const char *val)
{
    json_key(r, key);
    if (val) json_quote(&r->body, val);
    else     buf_append_str(&r->body, "null");
}

void cttp_json_int(cttp_response *r, const char *key, long long v)
{
    json_key(r, key);
    buf_printf(&r->body, "%lld", v);
}

void cttp_json_double(cttp_response *r, const char *key, double v)
{
    json_key(r, key);
    buf_printf(&r->body, "%g", v);
}

void cttp_json_bool(cttp_response *r, const char *key, int v)
{
    json_key(r, key);
    buf_append_str(&r->body, v ? "true" : "false");
}

void cttp_json_null(cttp_response *r, const char *key)
{
    json_key(r, key);
    buf_append_str(&r->body, "null");
}

/* Escape hatch: pre-formed fragments, e.g. an array you built by hand. */
void cttp_json_raw(cttp_response *r, const char *key, const char *raw)
{
    json_key(r, key);
    buf_append_str(&r->body, raw);
}

void cttp_json_obj_begin(cttp_response *r, const char *key)
{
    json_key(r, key);
    buf_append_str(&r->body, "{");
    if (r->_jd < 7) r->_jd++;
    r->_jf[r->_jd] = 1;
}

void cttp_json_arr_begin(cttp_response *r, const char *key)
{
    json_key(r, key);
    buf_append_str(&r->body, "[");
    if (r->_jd < 7) r->_jd++;
    r->_jf[r->_jd] = 1;
}

void cttp_json_obj_end(cttp_response *r)
{
    buf_append_str(&r->body, "}");
    if (r->_jd > 0) r->_jd--;
    r->_jf[r->_jd] = 0;
}

void cttp_json_arr_end(cttp_response *r)
{
    buf_append_str(&r->body, "]");
    if (r->_jd > 0) r->_jd--;
    r->_jf[r->_jd] = 0;
}

void cttp_json_end(cttp_response *r, int status)
{
    buf_append_str(&r->body, "}");
    r->status = status;
    responded(r);
}

void cttp_json_err(cttp_response *r, int status, const char *msg)
{
    cttp_json_begin(r);
    cttp_json_int(r, "code", status);
    cttp_json_str(r, "message", msg ? msg : cttp_status_text(status));
    cttp_json_end(r, status);
}
