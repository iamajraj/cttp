/* ==========================================================================
 * cttp.h — public surface of the cttp HTTP server library.
 *
 * READING GUIDE (read the files in this order):
 *   1. cttp.h   -> types: request / response / server / routes.
 *   2. buf.c    -> growable byte buffer (the memory backbone).
 *   3. http.c   -> the HTTP/1.1 engine: incremental parsing + responses.
 *   4. api.c    -> the friendly helpers handlers actually call.
 *   5. router.c -> URL routes, wildcards, middleware chain.
 *   6. static.c -> file serving: MIME, ETag, Range.
 *   7. server.c -> the poll() event loop (the heart).
 *   8. main.c   -> demo wiring.
 *
 * BIG PICTURE: one thread, non-blocking sockets, a poll() loop. Every TCP
 * connection is a tiny state machine:
 *
 *     READ headers -> READ body -> MIDDLEWARE -> ROUTE -> WRITE response
 *          ^                                                   |
 *          +__________________ keep-alive: loop _______________+
 * ========================================================================== */
#ifndef CTTP_H
#define CTTP_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* ---- growable byte buffer (learning source: internals/buf.c) ------------ */
typedef struct buf {
    char  *data;    /* malloc'd; NUL-terminated whenever we append          */
    size_t len;     /* bytes used (the NUL is not counted)                  */
    size_t cap;     /* bytes allocated                                     */
} buf_t;

/* ---- tunables (override with -D before including) ----------------------- */
#ifndef CTTP_MAX_HEADERS
#define CTTP_MAX_HEADERS   32          /* headers per request              */
#endif
#ifndef CTTP_MAX_HEADER_SZ
#define CTTP_MAX_HEADER_SZ (16*1024)   /* total header block cap           */
#endif
#ifndef CTTP_MAX_BODY_SZ
#define CTTP_MAX_BODY_SZ   (10*1024*1024) /* request body cap (10 MB)      */
#endif
#ifndef CTTP_MAX_PARAMS
#define CTTP_MAX_PARAMS    8           /* :name captures per route         */
#endif
#ifndef CTTP_MAX_ROUTES
#define CTTP_MAX_ROUTES    64
#endif
#ifndef CTTP_MAX_MIDDLEWARE
#define CTTP_MAX_MIDDLEWARE 16
#endif

#define CTTP_VERSION "2.0"

/* ---- HTTP methods -------------------------------------------------------- */
typedef enum {
    CTTP_GET, CTTP_HEAD, CTTP_POST, CTTP_PUT,
    CTTP_DELETE, CTTP_OPTIONS, CTTP_PATCH, CTTP_UNKNOWN
} cttp_method;

/* ---- an HTTP request ------------------------------------------------------
 * handers normally use helpers (cttp_header, cttp_param, cttp_query, ...)
 * rather than touching these fields directly.                             */
typedef struct cttp_request {
    cttp_method method;
    char        method_str[10];   /* "GET" as the client wrote it        */
    char        target[2048];     /* raw request target                  */
    char        path[1024];       /* percent-DECODED path, no query      */
    char        query[1024];      /* raw query string (decode on access) */
    int         version_minor;    /* 0 => HTTP/1.0, 1 => HTTP/1.1        */

    struct {                    /* headers as parsed name/value pairs   */
        char name[64];
        char value[768];
    } headers[CTTP_MAX_HEADERS];
    int nheaders;

    buf_t body;                   /* decoded body (CL or chunked TE)     */

    char params[CTTP_MAX_PARAMS][64];        /* :name of route params    */
    char pvalues[CTTP_MAX_PARAMS][256];      /* decoded values           */
    int  nparams;

    char req_id[24];              /* auto X-Request-Id (cttp_req_id)     */
    char cookie_buf[2048];        /* decoded cookie pairs workspace      */
    int  nccookies;               /* parsed cookie count                 */
    struct { char name[64]; char value[256]; } cookies[16];

    /* engine bookkeeping (do not touch) */
    int mw_cur;                   /* middleware cursor for cttp_next     */
    int responded;                /* set by any cttp_* response helper   */
} cttp_request;

/* ---- an HTTP response ---------------------------------------------------- */
typedef struct cttp_response {
    int   status;                 /* 200, 404, ...                       */
    char  ctype[64];              /* Content-Type for body               */
    buf_t body;                   /* payload                             */
    buf_t headers;                /* extra lines: "Name: value\r\n"      */
    int   chunked;                /* stream as chunked TE                */
    int   head_only;              /* HEAD: headers only                  */
    int   no_body;                /* 204/304: no body at all             */
    int   responded;              /* set by every cttp_* helper          */

    /* streaming writers (SSE) are wired by the engine: */
    int   stream;                 /* 1 => SSE push mode                  */
    void (*stream_write)(struct cttp_response *, const void *, size_t);
    void *stream_handle;          /* engine context (conn*)              */

    /* JSON-builder state (cttp_json_begin/.../cttp_json_end) */
    int  _jd;                     /* nesting depth                       */
    int  _jf[8];                  /* per-level "first item?" flags       */
} cttp_response;

/* A handler fills in the response via the cttp_* helpers. */
typedef void (*cttp_handler)(cttp_request *req, cttp_response *res);

/* Middleware wraps the rest of the chain: call cttp_next(req,res) to
 * continue, or just fill `res` yourself and skip it (short-circuit).     */
typedef void (*cttp_next)(cttp_request *, cttp_response *);
typedef void (*cttp_middleware)(cttp_request *req, cttp_response *res,
                                cttp_next next);

/* Cookie options; zero it for sane defaults (Path=/, session cookie).   */
typedef struct {
    int         max_age;          /* seconds; <=0 => session cookie      */
    const char *path;             /* default "/"                        */
    const char *domain;           /* default: host-only                 */
    const char *same_site;        /* "Strict"/"Lax"/"None" or NULL      */
    int         http_only;        /* hide from JS (document.cookie)     */
    int         secure;           /* HTTPS-only                         */
} cttp_cookie_opts;

/* ---- the server ---------------------------------------------------------- */
typedef struct cttp_server {
    int         listen_fd;        /* the passive listening socket        */
    char        host[64];
    int         port;
    const char *webroot;          /* directory for static files          */
    int         timeout_secs;     /* idle connections reaped after this  */

    struct pollfd *pfds;          /* parallel arrays pfds[i] <-> conns[i]*/
    struct conn  **conns;
    int nconns, cap;

    struct route {                /* pattern e.g. "/api/users/:id"       */
        char         pattern[256];
        cttp_method  method;
        cttp_handler handler;
    } routes[CTTP_MAX_ROUTES];
    int nroutes;

    cttp_middleware mw[CTTP_MAX_MIDDLEWARE];
    int nmw;

    void (*on_error)(cttp_request *, cttp_response *);   /* 404/405 hook */
    void (*on_log)(const cttp_request *, int status, size_t bytes);
    long req_counter;             /* for X-Request-Id                    */

    int running;                  /* cleared by SIGINT/SIGTERM           */
} cttp_server;

/* =============== public API =============== */

/* server.c — lifecycle */
int  cttp_init(cttp_server *s);                    /* set fields after    */
int  cttp_route(cttp_server *s, cttp_method m,
               const char *pattern, cttp_handler h);
void cttp_get(cttp_server *s, const char *p, cttp_handler h);
void cttp_head(cttp_server *s, const char *p, cttp_handler h);
void cttp_post(cttp_server *s, const char *p, cttp_handler h);
void cttp_put(cttp_server *s, const char *p, cttp_handler h);
void cttp_delete(cttp_server *s, const char *p, cttp_handler h);
void cttp_patch(cttp_server *s, const char *p, cttp_handler h);
void cttp_options(cttp_server *s, const char *p, cttp_handler h);
void cttp_use(cttp_server *s, cttp_middleware mw);
void cttp_on_error(cttp_server *s, void (*h)(cttp_request*, cttp_response*));
void cttp_on_log(cttp_server *s,
                 void (*h)(const cttp_request *, int status, size_t bytes));
void cttp_listen(cttp_server *s);                  /* blocks              */
void cttp_free(cttp_server *s);

/* api.c — request helpers */
const char *cttp_header(const cttp_request *r, const char *name);
const char *cttp_param(const cttp_request *r, const char *name);
const char *cttp_query(const cttp_request *r, const char *key);
int         cttp_query_has(const cttp_request *r, const char *key);
const char *cttp_form(const cttp_request *r, const char *key);
const char *cttp_cookie(const cttp_request *r, const char *name);
const char *cttp_req_id(const cttp_request *r);

/* api.c — response builders */
void cttp_send(cttp_response *r, int status, const char *ctype,
               const void *body, size_t len);
void cttp_text(cttp_response *r, int status, const char *fmt, ...);
void cttp_html(cttp_response *r, int status, const char *fmt, ...);
void cttp_no_content(cttp_response *r);
void cttp_redirect(cttp_response *r, int code, const char *location);
void cttp_set_header(cttp_response *r, const char *name, const char *value);
void cttp_set_cookie(cttp_response *r, const char *name, const char *value,
                     const cttp_cookie_opts *opts);
void cttp_delete_cookie(cttp_response *r, const char *name);
void cttp_etag(cttp_response *r, const char *tag);
void cttp_cache(cttp_response *r, int max_age_secs);
void cttp_cors(cttp_response *r, const char *origin,
               const char *methods, const char *headers);
void cttp_attachment(cttp_response *r, const char *filename);
void cttp_json_err(cttp_response *r, int status, const char *msg);

/* api.c — JSON builder with escaping */
void cttp_json_begin(cttp_response *r);
void cttp_json_str(cttp_response *r, const char *key, const char *val);
void cttp_json_int(cttp_response *r, const char *key, long long v);
void cttp_json_double(cttp_response *r, const char *key, double v);
void cttp_json_bool(cttp_response *r, const char *key, int v);
void cttp_json_null(cttp_response *r, const char *key);
void cttp_json_raw(cttp_response *r, const char *key, const char *raw);
void cttp_json_obj_begin(cttp_response *r, const char *key);
void cttp_json_obj_end(cttp_response *r);
void cttp_json_arr_begin(cttp_response *r, const char *key);
void cttp_json_arr_end(cttp_response *r);
void cttp_json_end(cttp_response *r, int status);

/* api.c — SSE push streaming */
void cttp_sse_start(cttp_response *r);
void cttp_sse_send(cttp_response *r, const char *event, const char *data);

/* api.c — URL & string & date utilities */
int         cttp_url_decode(char *dst, size_t n, const char *src, int plus);
const char *cttp_url_encode(char *dst, size_t n, const char *src);
const char *cttp_trim(char *s);
int         cttp_streq_i(const char *a, const char *b);
void        cttp_http_date(char *out, size_t n, time_t t);
time_t      cttp_parse_http_date(const char *s);
const char *cttp_status_text(int code);

/* logging (log.c) */
void cttp_log_info(const char *fmt, ...);
void cttp_log_error(const char *fmt, ...);

/* ---- engine internals (do not use in handlers) --------------------------- */
typedef enum {
    CONN_READ_HEADERS, CONN_READ_BODY, CONN_READ_CHUNK,
    CONN_WRITE, CONN_CLOSED
} conn_state;

typedef struct conn {
    int           fd;
    conn_state    state;
    buf_t         in;               /* read but not yet parsed            */
    buf_t         out;              /* response bytes not yet sent        */
    size_t        out_off;          /* how much of out[] is sent          */
    time_t        last_activity;
    cttp_request  req;
    long long     body_remaining;
    long long     chunk_rem;
    int           chunk_stage;
    int           expect_continue;
    int           want_continue;
    int           keep_alive;
    int           streamed;        /* SSE stream already opened          */
    conn_state    next_state;
} conn;

int  http_read_step(conn *c, cttp_server *s);
void http_init_request(cttp_request *r);
void http_free_request(cttp_request *r);
void http_finalize_response(conn *c, cttp_server *s, cttp_response *r);
void router_dispatch(cttp_server *s, cttp_request *req, cttp_response *res);
void static_serve(cttp_server *s, cttp_request *req, cttp_response *res);
void buf_append(buf_t *b, const void *p, size_t n);
void buf_append_str(buf_t *b, const char *s);
void buf_printf(buf_t *b, const char *fmt, ...);
void buf_consume(buf_t *b, size_t n);
void buf_free(buf_t *b);

#endif /* CTTP_H */

/* ================= one-file IMPLEMENTATION ================= */
#ifdef CTTP_IMPLEMENTATION

#include <stdarg.h>
#include <sys/stat.h>
#include <poll.h>
/* ==== from internals/buf.c ==== */
/* ==========================================================================
 * buf.c — growable byte buffer.
 *
 * Sockets deliver data in unpredictable chunk sizes, and an HTTP message
 * may arrive over many read() calls. Somewhere must accumulate bytes until
 * a full message exists — that is this struct (and it also builds every
 * outgoing response before writing).
 *
 * buf_printf is UNBOUNDED printf-to-buffer: it sizes the output first, so
 * handlers never need snprintf() (which forces fixed sizes on the caller).
 * ========================================================================== */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>



/* Guarantee room for `need` more bytes (amortized doubling). */
void buf_reserve(buf_t *b, size_t need)
{
    if (b->len + need + 1 <= b->cap) return;
    size_t newcap = b->cap ? b->cap : 4096;
    while (newcap < b->len + need + 1) newcap *= 2;
    b->data = realloc(b->data, newcap);
    if (!b->data) { perror("realloc"); exit(1); }
    b->cap = newcap;
}

/* Append raw bytes. Always NUL-terminates (not counted in len) so the
 * buffer can be passed straight to strcmp & friends: binary-safe. */
void buf_append(buf_t *b, const void *p, size_t n)
{
    if (n == 0) return;
    buf_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void buf_append_str(buf_t *b, const char *s) { buf_append(b, s, strlen(s)); }

/* printf-style append that grows the buffer to fit. */
void buf_vappendf(buf_t *b, const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);     /* measure first */
    va_end(ap2);
    if (n < 0) return;
    buf_reserve(b, (size_t)n);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    b->len += (size_t)n;
}

void buf_printf(buf_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    buf_vappendf(b, fmt, ap);
    va_end(ap);
}

/* Discard the first n bytes (used when a message is fully parsed and we
 * keep leftover bytes that belong to the next pipelined request). */
void buf_consume(buf_t *b, size_t n)
{
    if (n >= b->len) { b->len = 0; if (b->data) b->data[0] = '\0'; return; }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
    b->data[b->len] = '\0';
}

void buf_free(buf_t *b)
{
    free(b->data);
    b->data = NULL; b->len = 0; b->cap = 0;
}


/* ==== from internals/log.c ==== */
/* ==========================================================================
 * log.c — tiny timestamped logger (stderr). Swap vlog() for anything.
 * ========================================================================== */
#include <stdarg.h>
#include <stdio.h>
#include <time.h>



static void vlog(const char *tag, const char *fmt, va_list ap)
{
    char ts[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stderr, "[%s] %s ", ts, tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void cttp_log_info(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vlog("INFO", fmt, ap);
    va_end(ap);
}

void cttp_log_error(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vlog("ERROR", fmt, ap);
    va_end(ap);
}


/* ==== from internals/api.c ==== */
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


/* ==== from internals/http.c ==== */
/* ==========================================================================
 * http.c — the HTTP/1.1 engine: incremental parsing and response writing.
 *
 * Per connection this module drives a small state machine:
 *
 *   CONN_READ_HEADERS -> CONN_READ_BODY(/CHUNK) -> [middleware+route] -> WRITE
 *
 * Everything is incremental: a request may arrive in tiny TCP segments, so
 * parsing consumes only fully-arrived parts of conn->in and leaves the
 * rest untouched for later calls.
 * ========================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>



/* step results for the server loop */
#define STEP_NEED_MORE    0
#define STEP_RESP_READY   1
#define STEP_FATAL       -1

/* ---- small helpers ------------------------------------------------------- */

static const char *req_get_header(const cttp_request *r, const char *name)
{
    for (int i = 0; i < r->nheaders; i++)
        if (strcasecmp(r->headers[i].name, name) == 0)
            return r->headers[i].value;
    return NULL;
}

/* Is `token` a member of the (case-insensitive) comma list in v? */
static int has_token(const char *v, const char *token)
{
    size_t n = strlen(token);
    while (v && *v) {
        if (strncasecmp(v, token, n) == 0) return 1;
        const char *comma = strchr(v, ',');
        if (!comma) return 0;
        v = comma + 1;
        while (*v == ' ' || *v == '\t') v++;
    }
    return 0;
}

static cttp_method method_from_str(const char *s)
{
    if (!strcmp(s, "GET"))     return CTTP_GET;
    if (!strcmp(s, "HEAD"))    return CTTP_HEAD;
    if (!strcmp(s, "POST"))    return CTTP_POST;
    if (!strcmp(s, "PUT"))     return CTTP_PUT;
    if (!strcmp(s, "DELETE"))  return CTTP_DELETE;
    if (!strcmp(s, "OPTIONS")) return CTTP_OPTIONS;
    if (!strcmp(s, "PATCH"))   return CTTP_PATCH;
    return CTTP_UNKNOWN;
}

/* ---- request/response lifecycle ------------------------------------------ */

void http_init_request(cttp_request *r)
{
    memset(r, 0, sizeof *r);
    r->nccookies = -1;            /* -1 = "not parsed yet" sentinel */
    r->mw_cur = -1;               /* middleware cursor              */
}

void http_free_request(cttp_request *r) { buf_free(&r->body); }

const char *cttp_status_text(int code)
{
    switch (code) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 413: return "Content Too Large";
    case 416: return "Range Not Satisfiable";
    case 417: return "Expectation Failed";
    case 422: return "Unprocessable Content";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default:  return "OK";
    }
}

/* RFC 9110 §5.6.7: "Sun, 06 Nov 1994 08:49:37 GMT" */
void cttp_http_date(char *out, size_t n, time_t t)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    static const char *days[] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
    static const char *mons[] = { "Jan","Feb","Mar","Apr","May","Jun",
                                  "Jul","Aug","Sep","Oct","Nov","Dec" };
    snprintf(out, n, "%s, %02d %s %d %02d:%02d:%02d GMT",
             days[tm.tm_wday], tm.tm_mday, mons[tm.tm_mon],
             tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

time_t cttp_parse_http_date(const char *s)
{
    /* Accepts exactly the format our cttp_http_date() emits — enough for
     * comparing dates we ourselves produced (e.g. Last-Modified). */
    static const char *mons[] = { "Jan","Feb","Mar","Apr","May","Jun",
                                  "Jul","Aug","Sep","Oct","Nov","Dec" };
    struct tm tm = {0};
    char mon[4] = {0};
    if (sscanf(s, "%*s %d %3s %d %d:%d:%d",
               &tm.tm_mday, mon, &tm.tm_year,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
        return 0;
    for (int i = 0; i < 12; i++)
        if (strcmp(mon, mons[i]) == 0) { tm.tm_mon = i; break; }
    tm.tm_year -= 1900;
    tm.tm_isdst = 0;
    return timegm(&tm);
}

/* ---- percent-decoding ------------------------------------------------------
 * URLs arrive percent-encoded ("%20" for space). Decoding happens once at
 * parse time so handlers and the router see plain text. Decoding also means
 * "%2e%2e" becomes ".." BEFORE the traversal check sees it — which is why
 * the safety check must run on the decoded value (see static.c).
 * `plus` decodes '+' as space (needed for query strings / forms, not paths). */
int cttp_url_decode(char *dst, size_t n, const char *src, int plus)
{
    size_t o = 0;
    for (size_t i = 0; src[i]; ) {
        char c = src[i];
        if (c == '%' && src[i+1] && src[i+2]) {           /* %XX escape   */
            char hex[3] = { src[i+1], src[i+2], 0 };
            char *end;
            long v = strtol(hex, &end, 16);
            if (end != hex + 2) return -1;                /* not hex      */
            c = (char)v;
            i += 3;
        } else if (c == '+' && plus) {
            c = ' '; i++;
        } else {
            i++;
        }
        if (o + 1 >= n) return -1;                        /* dst too small*/
        dst[o++] = c;
    }
    dst[o] = '\0';
    return 0;
}

const char *cttp_url_encode(char *dst, size_t n, const char *src)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; src[i]; i++) {
        unsigned char c = (unsigned char)src[i];
        int safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') ||
                   c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            if (o + 1 >= n) return NULL;
            dst[o++] = (char)c;
        } else {
            if (o + 3 >= n) return NULL;
            dst[o++] = '%'; dst[o++] = hex[c >> 4]; dst[o++] = hex[c & 15];
        }
    }
    dst[o] = '\0';
    return dst;
}

/* ---- building the response bytes ------------------------------------------
 * The engine writes: status line, standard headers, then any lines the
 * handler added via cttp_set_header/cookie/cors (they live in res->headers
 * as ready-made "Name: value\r\n" lines), then the body (with chunked TE
 * for streaming, nothing for HEAD / 204 / 304).                              */

static void decide_keep_alive(conn *c)
{
    int keep = (c->req.version_minor >= 1);      /* HTTP/1.1 default: yes   */
    const char *h = req_get_header(&c->req, "Connection");
    if (h) {
        if (has_token(h, "close"))      keep = 0;
        if (has_token(h, "keep-alive")) keep = 1;
    } else if (c->req.version_minor == 0) {
        keep = 0;
    }
    c->keep_alive = keep;
}

void http_finalize_response(conn *c, cttp_server *s, cttp_response *res)
{
    buf_t *o = &c->out;
    char date[64];
    (void)s;

    /* SSE already sent the header block; only close the chunk stream. */
    if (c->streamed) {
        buf_append_str(o, "0\r\n\r\n");
        cttp_log_info("%s %s -> SSE stream", c->req.method_str, c->req.path);
        c->state = CONN_WRITE;
        return;
    }

    decide_keep_alive(c);
    cttp_http_date(date, sizeof date, time(NULL));

    buf_printf(o, "HTTP/1.1 %d %s\r\n", res->status, cttp_status_text(res->status));
    buf_printf(o, "Server: cttp/%s\r\nServer-Request-Id: %s\r\n",
               CTTP_VERSION, c->req.req_id);
    buf_printf(o, "Date: %s\r\n", date);

    int no_body = res->no_body || res->status == 204 || res->status == 304 ||
                  res->status < 200;

    if (!no_body) {
        if (res->chunked) {
            buf_append_str(o, "Transfer-Encoding: chunked\r\n");
        } else {
            buf_printf(o, "Content-Type: %s\r\n",
                       res->ctype[0] ? res->ctype : "text/plain");
            buf_printf(o, "Content-Length: %zu\r\n", res->body.len);
        }
    }

    if (!c->keep_alive)
        buf_append_str(o, "Connection: close\r\n");

    buf_append(o, res->headers.data, res->headers.len);  /* handler headers */
    buf_append_str(o, "\r\n");                          /* blank line      */

    if (no_body) { c->state = CONN_WRITE; return; }

    if (res->chunked) {
        for (size_t i = 0; i < res->body.len; i += 4096) {
            size_t n = res->body.len - i > 4096 ? 4096 : res->body.len - i;
            buf_printf(o, "%zx\r\n", n);
            buf_append(o, res->body.data + i, n);
            buf_append_str(o, "\r\n");
        }
        buf_append_str(o, "0\r\n\r\n");
    } else if (!res->head_only) {
        /* HEAD: Content-Length above reflects a GET; body stays silent. */
        buf_append(o, res->body.data, res->body.len);
    }

    c->state = CONN_WRITE;
}

/* ---- request-line and header parsing -------------------------------------- */

/* Split "GET /a/b?x=1 HTTP/1.1". 0 ok / -1 malformed / -2 bad version. */
static int parse_request_line(char *line, cttp_request *r)
{
    char *sp1 = strchr(line, ' ');
    char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (!sp1 || !sp2) return -1;
    *sp1 = '\0'; *sp2 = '\0';

    snprintf(r->method_str, sizeof r->method_str, "%s", line);
    r->method = method_from_str(line);
    snprintf(r->target, sizeof r->target, "%s", sp1 + 1);

    const char *ver = sp2 + 1;
    if (strncmp(ver, "HTTP/1.", 7) != 0) return -2;
    if (ver[7] == '0' && ver[8] == '\0')      r->version_minor = 0;
    else if (ver[7] == '1' && ver[8] == '\0') r->version_minor = 1;
    else return -2;

    /* Split target into decoded path + raw query. */
    char raw_path[1024];
    const char *q = strchr(r->target, '?');
    if (q) {
        snprintf(raw_path, sizeof raw_path, "%.*s", (int)(q - r->target), r->target);
        snprintf(r->query, sizeof r->query, "%s", q + 1);
    } else {
        snprintf(raw_path, sizeof raw_path, "%s", r->target);
        r->query[0] = '\0';
    }
    if (cttp_url_decode(r->path, sizeof r->path, raw_path, 0) != 0)
        return -1;
    return 0;
}

/* Parse a complete header block from conn->in.
 * 1 = parsed, 0 = incomplete, -1 = malformed/flood. */
static int parse_headers(conn *c)
{
    buf_t *in = &c->in;
    cttp_request *r = &c->req;
    const char *data = in->data;
    size_t len = in->len;

    /* 1. Wait until the terminator "\r\n\r\n" has arrived. */
    if (len < 4) return 0;
    const char *end = NULL;
    for (size_t i = 0; i + 3 < len; i++)
        if (memcmp(data + i, "\r\n\r\n", 4) == 0) { end = data + i; break; }
    if (!end) return len > CTTP_MAX_HEADER_SZ ? -1 : 0;

    size_t block_len = (size_t)(end - data) + 4;
    if (block_len > CTTP_MAX_HEADER_SZ) return -1;

    /* 2. Scratch copy: lines must be NUL-terminated to parse in place. */
    char *block = malloc(block_len + 1);
    memcpy(block, data, block_len);
    block[block_len] = '\0';

    /* 3. Request line. */
    char *p = block, *cr = strchr(block, '\r');
    if (!cr || cr[1] != '\n') { free(block); return -1; }
    *cr = '\0';
    if (parse_request_line(p, r) != 0) { free(block); return -1; }
    p = cr + 2;

    /* 4. Header lines "Name: value" until the empty line. */
    while (*p) {
        if (p[0] == '\r' && p[1] == '\n') break;    /* end of headers */
        char *nl = strstr(p, "\r\n");
        if (!nl) { free(block); return -1; }
        *nl = '\0';
        if (r->nheaders >= CTTP_MAX_HEADERS) { free(block); return -1; }
        char *colon = strchr(p, ':');
        if (!colon) { free(block); return -1; }
        *colon = '\0';
        const char *val = colon + 1;
        while (*val == ' ' || *val == '\t') val++;
        snprintf(r->headers[r->nheaders].name,  64,  "%s", p);
        snprintf(r->headers[r->nheaders].value, 768, "%s", val);
        r->nheaders++;
        p = nl + 2;
    }

    buf_consume(in, block_len);
    free(block);
    return 1;
}

/* ---- body handling --------------------------------------------------------- */

/* RFC 9111 §10.1.1: acknowledge Expect: 100-continue before reading. */
static void send_continue_100(conn *c)
{
    buf_append_str(&c->out, "HTTP/1.1 100 Continue\r\n\r\n");
    c->want_continue = 1;
    c->next_state = c->state;
    c->state = CONN_WRITE;
}

static int read_body_clength(conn *c)
{
    long long avail = (long long)c->in.len;
    long long take  = c->body_remaining < avail ? c->body_remaining : avail;
    if (take > 0) {
        buf_append(&c->req.body, c->in.data, (size_t)take);
        buf_consume(&c->in, (size_t)take);
        c->body_remaining -= take;
    }
    return c->body_remaining == 0;
}

/* Chunked decoding (RFC 9112 §7.1): "<hex>[;ext]\r\n<data>\r\n" ... "0\r\n\r\n"
 * chunk_stage: 0 size line, 1 data, 2 trailer. 1 done, 0 need more, <0 error */
static int read_body_chunked(conn *c)
{
    buf_t *in = &c->in;

    for (;;) {
        const char *data = in->data;
        size_t len = in->len;

        if (c->chunk_stage == 0) {
            const char *nl = memchr(data, '\n', len);
            if (!nl) return len > 8192 ? -1 : 0;
            size_t line_len = (size_t)(nl - data) + 1;
            char line[64];
            if (line_len >= sizeof line) return -1;
            memcpy(line, data, line_len - 1);
            line[line_len - 1] = '\0';
            buf_consume(in, line_len);

            char *semi = strchr(line, ';');
            if (semi) *semi = '\0';
            unsigned long long sz = strtoull(line, NULL, 16);
            if (c->req.body.len + (size_t)sz > CTTP_MAX_BODY_SZ) return -2;
            c->chunk_rem = (long long)sz;
            c->chunk_stage = sz ? 1 : 2;
        }
        else if (c->chunk_stage == 1) {
            if (c->chunk_rem == 0) {                 /* expect CRLF line  */
                if (len < 2) return 0;
                if (data[0] != '\r' || data[1] != '\n') return -1;
                buf_consume(in, 2);
                c->chunk_stage = 0;
            } else {
                size_t take = (size_t)c->chunk_rem;
                if (take > len) take = len;
                buf_append(&c->req.body, data, take);
                buf_consume(in, take);
                c->chunk_rem -= (long long)take;
            }
        }
        else {                                       /* trailer section   */
            if (len == 0) return 0;
            if (len >= 2 && data[0] == '\r' && data[1] == '\n') {
                buf_consume(in, 2);
                return 1;
            }
            const char *nl = memchr(data, '\n', len);
            if (!nl) return len > 8192 ? -1 : 0;
            buf_consume(in, (size_t)(nl - data) + 1);
        }
    }
}

/* Pick body mode from headers; 0 = reading body, 1 = no body. <0 error. */
static int begin_body(conn *c)
{
    cttp_request *r = &c->req;

    const char *te = req_get_header(r, "Transfer-Encoding");
    if (te && has_token(te, "chunked")) {
        c->chunk_stage = 0;
        c->chunk_rem = 0;
        c->state = CONN_READ_CHUNK;
        return 0;
    }
    const char *cl = req_get_header(r, "Content-Length");
    if (cl) {
        char *endp;
        long long n = strtoll(cl, &endp, 10);
        if (endp == cl || *endp != '\0' || n < 0) return -1;
        if (n > CTTP_MAX_BODY_SZ) return -2;
        c->body_remaining = n;
        c->state = CONN_READ_BODY;
        return 0;
    }
    return 1;
}

/* request helpers declared in cttp.h are implemented in api.c; engine needs
 * req_get_header externally for the middleware dispatch, so expose: */
const char *cttp_header(const cttp_request *r, const char *name)
{
    return req_get_header(r, name);
}

/* ---- the read state machine ------------------------------------------------ */

static void dispatch_request(conn *c, cttp_server *s);

/* Bad request / oversized bodies etc: build the error response now. */
static int error_response(conn *c, int status, const char *msg)
{
    cttp_response res;
    memset(&res, 0, sizeof res);
    cttp_json_err(&res, status, msg);
    http_finalize_response(c, NULL, &res);
    buf_free(&res.body); buf_free(&res.headers);
    return STEP_RESP_READY;
}

int http_read_step(conn *c, cttp_server *s)
{
    for (;;) {
        switch (c->state) {

        case CONN_READ_HEADERS: {
            int pr = parse_headers(c);
            if (pr == 0) return STEP_NEED_MORE;
            if (pr < 0)  return error_response(c, 400, "malformed request");

            cttp_request *r = &c->req;
            if (r->method == CTTP_UNKNOWN)
                return error_response(c, 501, "method not implemented");

            int b = begin_body(c);
            if (b < 0)
                return error_response(c, b == -2 ? 413 : 400,
                                      b == -2 ? "request body too large"
                                              : "bad content-length");
            if (b == 1) { dispatch_request(c, s); return STEP_RESP_READY; }

            const char *expect = req_get_header(r, "Expect");
            if (expect && strcasecmp(expect, "100-continue") == 0) {
                send_continue_100(c);
                return STEP_NEED_MORE;
            }
            continue;
        }

        case CONN_READ_BODY:
        case CONN_READ_CHUNK: {
            int done = (c->state == CONN_READ_BODY)
                         ? read_body_clength(c)
                         : read_body_chunked(c);
            if (done < 0)
                return error_response(c, done == -2 ? 413 : 400,
                                      done == -2 ? "request body too large"
                                                 : "malformed chunked body");
            if (done == 0) return STEP_NEED_MORE;
            dispatch_request(c, s);
            return STEP_RESP_READY;
        }

        case CONN_WRITE:
        case CONN_CLOSED:
            return STEP_NEED_MORE;
        }
    }
}

/* Request complete: middleware -> route -> build the response bytes. */
static void dispatch_request(conn *c, cttp_server *s)
{
    cttp_response res;
    memset(&res, 0, sizeof res);
    res.head_only = (c->req.method == CTTP_HEAD);
    res.stream_handle = c;             /* SSE writers reach the socket */

    router_dispatch(s, &c->req, &res);

    /* access-log hook fires once per request, right before the bytes go  */
    if (s && s->on_log && c->req.responded)
        s->on_log(&c->req, res.status, c->streamed ? 0 : res.body.len);

    http_finalize_response(c, s, &res);

    buf_free(&res.body);
    buf_free(&res.headers);
}


/* ==== from internals/router.c ==== */
/* ==========================================================================
 * router.c — URL routing: ":params", wildcards, middleware, custom 404.
 *
 * A pattern like "/api/users/:id/files" + a wildcard segment, against the request path "/api/users/42/files/a.txt"
 * captures:  id = "42",  … = "a.txt" (wildcard = rest of the path).
 *
 * Matching walks pattern and path one '/'-segment at a time; ':' captures
 * one percent-decoded segment, '*' swallows the remainder.
 * ========================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>



/* The middleware chain needs cttp_next() to find the server. The loop is
 * single-threaded, so one static pointer set right before dispatch is
 * safe and simple (larger frameworks thread a context through instead). */
static cttp_server *g_router;

/* Hand the request to the next middleware in the chain; when the chain
 * runs out, control returns to router_dispatch which runs the route.  */
static void mw_next(cttp_request *req, cttp_response *res)
{
    cttp_server *s = g_router;
    int i = req->mw_cur + 1;
    if (i < s->nmw) {
        req->mw_cur = i;
        s->mw[i](req, res, mw_next);
    }
}

/* Copy the next '/'-separated segment from *s, advancing the cursor and
 * reporting where the segment started. 1 = segment read, 0 = end.       */
static int next_seg(const char **s, const char **start,
                    char *out, size_t n)
{
    const char *p = *s;
    while (*p == '/') p++;
    *start = p;
    if (!*p) { out[0] = '\0'; *s = p; return 0; }
    const char *e = strchr(p, '/');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len >= n) len = n - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    *s = e ? e + 1 : p + len;
    return 1;
}

/* One route vs one request. 1 = match (captures set), 2 = path matches
 * but the method differs (drives a precise 405), 0 = no match.          */
static int route_match(int ridx, cttp_request *req)
{
    const char *p = g_router->routes[ridx].pattern;
    char pathcopy[1024];                    /* walk a scratch copy    */
    snprintf(pathcopy, sizeof pathcopy, "%s", req->path);
    const char *q = pathcopy, *qseg_start = pathcopy;

    char pseg[128], qseg[512];
    int np = 0;
    int result = 0;    /* 0 = no match, 1 = path + wildcard/direct hit */


    for (;;) {
        const char *pstart = p;
        int has_p = next_seg(&p, &pstart, pseg, sizeof pseg);
        int has_q = next_seg(&q, &qseg_start, qseg, sizeof qseg);

        if (!has_p && !has_q) { result = 1; break; }   /* both done */

        if (has_p != has_q) {                 /* one side ran out       */
            /* "/files/" + wildcard with nothing left: value = ""  */
            if (has_p && pseg[0] == '*' && np < CTTP_MAX_PARAMS) {
                cttp_request *rw = (cttp_request *)req;
                snprintf(rw->params[np], 64, "%s", pseg);
                rw->pvalues[np][0] = '\0';
                np++;
                result = 1;
            }
            break;
        }

        cttp_request *rw = (cttp_request *)req;
        if (pseg[0] == ':') {                /* one-segment capture    */
            if (np >= CTTP_MAX_PARAMS) break;
            snprintf(rw->params[np], 64, "%s", pseg + 1);
            if (cttp_url_decode(rw->pvalues[np], 256, qseg, 0) != 0)
                break;
            np++;
    } else if (pseg[0] == '*') {         /* swallow what's left    */
            if (np >= CTTP_MAX_PARAMS) break;
            snprintf(rw->params[np], 64, "%s", pseg);
            /* rest starts AT the current path segment (qseg_start) */
            if (cttp_url_decode(rw->pvalues[np], 256, qseg_start, 0) != 0)
                break;
            np++;
            result = 1;
            break;
        } else if (strcmp(pseg, qseg) != 0) {
            break;                           /* literal mismatch       */
        }
    }

    if (result != 0) {
        cttp_request *rw = (cttp_request *)req;
        /* commit captured params into the request */
        rw->nparams = np;
        /* HEAD is automatically served by GET handlers (the engine strips
     * the body when res.head_only is set) — RFC 9110 §9.3.2 behaviour. */
    cttp_method rm = g_router->routes[ridx].method;
    int ok = rm == req->method ||
             (req->method == CTTP_HEAD && rm == CTTP_GET);
    return ok ? 1 : 2;
    }
    return 0;
}

/* Middleware, then route, with sane fallbacks:
 *   OPTIONS without registration -> Allow: list (RFC 9110 §9.3.7)
 *   path exists, wrong method    -> 405 + Allow (always engine-made)
 *   GET/HEAD with a webroot      -> filesystem; missing file -> hook 404
 *   otherwise                    -> on_error hook or default JSON 404 */
void router_dispatch(cttp_server *s, cttp_request *req, cttp_response *res)
{
    g_router = s;
    req->mw_cur = -1;

    if (s->nmw > 0) {                     /* enter the chain         */
        req->mw_cur = 0;
        s->mw[0](req, res, mw_next);
    }

    if (res->responded)                   /* short-circuited: done   */
        return;

    int method_mismatch = 0;
    for (int i = 0; i < s->nroutes; i++) {
        int m = route_match(i, req);
        if (m == 1) {
            s->routes[i].handler(req, res);
            return;
        }
        if (m == 2) method_mismatch = 1;
    }

    if (req->method == CTTP_OPTIONS) {
        res->status = 200;
        cttp_set_header(res, "Allow", "GET, HEAD, POST, PUT, DELETE, OPTIONS");
        res->responded = 1;
        return;
    }

    if (method_mismatch) {                /* engine-owned, like OPTIONS */
        cttp_json_err(res, 405, "method not allowed");
        cttp_set_header(res, "Allow", "GET, HEAD, POST, PUT, DELETE, OPTIONS");
        res->responded = 1;
        return;
    }

    if ((req->method == CTTP_GET || req->method == CTTP_HEAD) && s->webroot) {
        static_serve(s, req, res);        /* filesystem fallback        */
        res->responded = 1;               /* static_serve always answers */
    }

    if (!res->responded) {
        if (s->on_error) s->on_error(req, res);
        else            cttp_json_err(res, 404, "resource not found");
        res->responded = 1;
    }
}


/* ==== from internals/static.c ==== */
/* ==========================================================================
 * static.c — serving files from the webroot.
 *
 * Beyond open()+read()+write() this module layers on the modern bits:
 *   * path traversal protection  ("/../etc/passwd" must never escape)
 *   * MIME types by file extension
 *   * ETag + If-None-Match  ->  304 Not Modified
 *   * Range requests        ->  206 Partial Content (download resume)
 * ========================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>



/* MIME table: enough extensions to feel complete without bloat. */
static const struct { const char *ext, *mime; } MIMES[] = {
    { ".html", "text/html; charset=utf-8" },
    { ".htm",  "text/html; charset=utf-8" },
    { ".css",  "text/css; charset=utf-8" },
    { ".js",   "text/javascript; charset=utf-8" },
    { ".json", "application/json" },
    { ".txt",  "text/plain; charset=utf-8" },
    { ".png",  "image/png" },
    { ".jpg",  "image/jpeg" },
    { ".jpeg", "image/jpeg" },
    { ".gif",  "image/gif" },
    { ".svg",  "image/svg+xml" },
    { ".ico",  "image/x-icon" },
    { ".webp", "image/webp" },
    { ".woff", "font/woff" },
    { ".woff2","font/woff2" },
    { ".pdf",  "application/pdf" },
    { ".zip",  "application/zip" },
    { ".wasm", "application/wasm" },
    { ".mp3",  "audio/mpeg" },
    { ".mp4",  "video/mp4" },
};
#define NMIMES (int)(sizeof MIMES / sizeof MIMES[0])

static const char *mime_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    for (int i = 0; i < NMIMES; i++)
        if (strcasecmp(dot, MIMES[i].ext) == 0)
            return MIMES[i].mime;
    return "application/octet-stream";
}

/* Reject traversal attempts. The path is percent-decoded by the parser,
 * which is exactly why this check runs on req->path: "%2e%2e" has already
 * become ".." here, so encoded trips are caught by the same test that
 * catches literal ones.                                             */
static int is_unsafe_path(const char *p)
{
    if (!p || p[0] != '/') return 1;
    if (strstr(p, "..")) return 1;
    return 0;
}

/* Read the whole open file. Heap buffer of exactly *out_len bytes. */
static char *read_whole_file(int fd, size_t *out_len)
{
    struct stat st;
    if (fstat(fd, &st) < 0) return NULL;
    size_t len = (size_t)st.st_size;
    char *data = malloc(len + 1);
    if (!data) return NULL;
    size_t total = 0;
    while (total < len) {
        ssize_t n = read(fd, data + total, len - total);
        if (n < 0) { if (errno == EINTR) continue; free(data); return NULL; }
        if (n == 0) break;             /* EOF early: trust read() */
        total += (size_t)n;
    }
    *out_len = total;
    data[total] = '\0';
    return data;
}

void static_serve(cttp_server *s, cttp_request *req, cttp_response *res)
{
    char path[2048];

    if (!s->webroot || is_unsafe_path(req->path)) {
        cttp_json_err(res, 403, "path not allowed");
        return;
    }

    /* URL path -> filesystem path (index.html for directories). */
    if (strcmp(req->path, "/") == 0)
        snprintf(path, sizeof path, "%s/index.html", s->webroot);
    else if (req->path[strlen(req->path) - 1] == '/')
        snprintf(path, sizeof path, "%s%sindex.html", s->webroot, req->path);
    else
        snprintf(path, sizeof path, "%s%s", s->webroot, req->path);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        cttp_json_err(res, 404, "resource not found");
        return;
    }
    struct stat st;
    fstat(fd, &st);
    if (S_ISDIR(st.st_mode)) {          /* "/dir" without the slash       */
        close(fd);
        snprintf(path, sizeof path, "%s%s/index.html", s->webroot, req->path);
        fd = open(path, O_RDONLY);
        if (fd < 0) { cttp_json_err(res, 404, "resource not found"); return; }
        fstat(fd, &st);
    }

    /* ETag = size + mtime. Same string => browser cache still valid. */
    char etag[64];
    snprintf(etag, sizeof etag, "\"%zx-%zx\"",
             (size_t)st.st_size, (size_t)st.st_mtime);

    const char *inm = cttp_header(req, "If-None-Match");
    if (inm && strstr(inm, etag)) {
        close(fd);
        res->status = 304;
        res->no_body = 1;
        cttp_set_header(res, "ETag", etag);
        cttp_cache(res, 3600);
        return;
    }

    /* Range: bytes=N-M, satisfied with a 206 slice. */
    const char *rh = cttp_header(req, "Range");
    if (rh) {
        unsigned long long start, end, total = (unsigned long long)st.st_size;
        if (sscanf(rh, "bytes=%llu-%llu", &start, &end) == 2 &&
            start < total && end >= start) {
            if (end >= total) end = total - 1;
        } else if (sscanf(rh, "bytes=%llu-", &start) == 1 && start < total) {
            end = total - 1;
        } else {
            /* unsatisfiable: RFC 9110 §14.2 wants the real size back */
            char cr[64];
            snprintf(cr, sizeof cr, "bytes */%llu", total);
            cttp_set_header(res, "Content-Range", cr);
            res->status = 416;
            res->no_body = 1;
            close(fd);
            return;
        }

        lseek(fd, (off_t)start, SEEK_SET);
        size_t len = (size_t)(end - start + 1);
        char *data = malloc(len);
        size_t got = 0;
        while (got < len) {
            ssize_t n = read(fd, data + got, len - got);
            if (n < 0) { if (errno == EINTR) continue; break; }
            if (n == 0) break;
            got += (size_t)n;
        }
        close(fd);

        res->status = 206;
        snprintf(res->ctype, sizeof res->ctype, "%s", mime_for(path));
        char cr[64];
        snprintf(cr, sizeof cr, "bytes %llu-%llu/%llu",
                 start, start + got - 1, total);
        cttp_set_header(res, "Content-Range", cr);
        buf_free(&res->body);
        buf_append(&res->body, data, got);
        free(data);
        return;
    }

    /* Plain 200 with the whole file. */
    size_t len = 0;
    char *data = read_whole_file(fd, &len);
    close(fd);
    if (!data) { cttp_json_err(res, 500, "read failed"); return; }

    res->status = 200;
    snprintf(res->ctype, sizeof res->ctype, "%s", mime_for(path));
    cttp_set_header(res, "ETag", etag);
    cttp_cache(res, 3600);
    buf_free(&res->body);
    buf_append(&res->body, data, len);
    free(data);
}


/* ==== from internals/server.c ==== */
/* ==========================================================================
 * server.c — sockets + the poll() event loop.
 *
 * ARCHITECTURE
 * ------------
 * One thread multiplexes every socket with poll() on non-blocking fds.
 * Nothing blocks: read()/write()/accept() return EAGAIN when the kernel
 * can't proceed, and the loop resumes when poll() says the socket is
 * ready. pfds[] and conns[] are parallel arrays; pfds[0] is the listener.
 * ========================================================================== */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>



#define LISTEN_BACKLOG   128
#define IO_CHUNK         16384    /* bytes per read()/write() attempt   */
#define POLL_INTERVAL_MS 1000     /* wake up at least this often        */

/* Signal handlers get no arguments; the loop checks this flag instead.
 * (fancier designs use the "self-pipe" trick to wake poll() instantly). */
static cttp_server *g_server;
static void handle_signal(int sig)
{
    (void)sig;
    if (g_server) g_server->running = 0;
}

/* ---- connection table -------------------------------------------------- */

static void table_reserve(cttp_server *s, int need)
{
    if (need <= s->cap) return;
    int newcap = s->cap ? s->cap : 16;
    while (newcap < need) newcap *= 2;
    s->pfds  = realloc(s->pfds,  (size_t)newcap * sizeof *s->pfds);
    s->conns = realloc(s->conns, (size_t)newcap * sizeof *s->conns);
    if (!s->pfds || !s->conns) { perror("realloc"); exit(1); }
    s->cap = newcap;
}

/* O_NONBLOCK sockets return EAGAIN instead of stalling the whole server. */
static void set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Reset a connection slot for the next keep-alive request. */
static void conn_reset_for_next(conn *c, cttp_server *s)
{
    http_free_request(&c->req);
    http_init_request(&c->req);
    c->out_off        = 0;
    c->body_remaining = 0;
    c->chunk_rem      = 0;
    c->chunk_stage    = 0;
    c->keep_alive     = 0;
    c->streamed       = 0;
    c->want_continue  = 0;
    c->state          = CONN_READ_HEADERS;
    buf_consume(&c->out, c->out.len);

    /* X-Request-Id: unique per request, visible in responses and logs. */
    s->req_counter++;
    snprintf(c->req.req_id, sizeof c->req.req_id, "%zx-%zx",
             (size_t)time(NULL), (size_t)s->req_counter);
    /* NOTE: c->in is NOT emptied — bytes the client already sent for
     * the next pipelined request survive for the next parse. */
}

static void conn_close(cttp_server *s, int idx)
{
    conn *c = s->conns[idx];
    close(c->fd);
    http_free_request(&c->req);
    buf_free(&c->in);
    buf_free(&c->out);
    free(c);

    /* O(1) removal by moving the last slot into idx (the loop tolerates
     * re-checking the replacement slot). */
    s->conns[idx] = s->conns[s->nconns - 1];
    s->pfds[idx]  = s->pfds[s->nconns - 1];
    s->nconns--;
}

/* ---- accepting connections ---------------------------------------------- */

static void accept_new_clients(cttp_server *s)
{
    for (;;) {
        struct sockaddr_in addr;
        socklen_t alen = sizeof addr;
        int fd = accept(s->listen_fd, (struct sockaddr *)&addr, &alen);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            perror("accept");
            return;
        }

        set_nonblock(fd);
        /* Nagle batches small writes (~40ms latency); responses want it off */
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        table_reserve(s, s->nconns + 1);
        conn *c = calloc(1, sizeof *c);
        c->fd = fd;
        c->state = CONN_READ_HEADERS;
        c->last_activity = time(NULL);
        http_init_request(&c->req);
        s->conns[s->nconns] = c;
        s->pfds[s->nconns]  = (struct pollfd){ .fd = fd, .events = POLLIN };
        s->nconns++;
        conn_reset_for_next(c, s);      /* request id + fresh state */
    }
}

/* ---- flushing outgoing bytes --------------------------------------------- */

/* Write what the kernel accepts right now; 0 = kernel full, -1 gone. */
static int flush_conn(conn *c)
{
    while (c->out_off < c->out.len) {
        ssize_t n = send(c->fd, c->out.data + c->out_off,
                         c->out.len - c->out_off, MSG_NOSIGNAL);
        if (n > 0) { c->out_off += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;                   /* retry when POLLOUT is ready */
        return -1;
    }
    return 1;                           /* fully flushed */
}

static void on_flush_complete(cttp_server *s, int idx)
{
    conn *c = s->conns[idx];

    if (c->want_continue) {             /* 100 Continue just went out */
        c->want_continue = 0;
        c->state = c->next_state;
        c->out_off = 0;
        buf_consume(&c->out, c->out.len);
        return;
    }

    if (c->keep_alive) {
        conn_reset_for_next(c, s);      /* ready for the next request */
        return;
    }
    conn_close(s, idx);                 /* Connection: close */
}

/* ---- config & routes ----------------------------------------------------- */

int cttp_init(cttp_server *s)
{
    memset(s, 0, sizeof *s);
    /* friendly defaults; override the fields before cttp_listen */
    snprintf(s->host, sizeof s->host, "127.0.0.1");
    s->port         = 8080;
    s->timeout_secs = 30;
    s->listen_fd    = -1;
    return 0;
}

static int route_add(cttp_server *s, cttp_method m,
                     const char *pattern, cttp_handler h)
{
    if (s->nroutes >= CTTP_MAX_ROUTES) {
        cttp_log_error("route limit reached: %s", pattern);
        return -1;
    }
    snprintf(s->routes[s->nroutes].pattern, 256, "%s", pattern);
    s->routes[s->nroutes].method  = m;
    s->routes[s->nroutes].handler = h;
    s->nroutes++;
    return 0;
}

int cttp_route(cttp_server *s, cttp_method m,
               const char *pattern, cttp_handler h)
{
    return route_add(s, m, pattern, h);
}

void cttp_get(cttp_server *s, const char *p, cttp_handler h)
{ route_add(s, CTTP_GET, p, h); }
void cttp_head(cttp_server *s, const char *p, cttp_handler h)
{ route_add(s, CTTP_HEAD, p, h); }
void cttp_post(cttp_server *s, const char *p, cttp_handler h)
{ route_add(s, CTTP_POST, p, h); }
void cttp_put(cttp_server *s, const char *p, cttp_handler h)
{ route_add(s, CTTP_PUT, p, h); }
void cttp_delete(cttp_server *s, const char *p, cttp_handler h)
{ route_add(s, CTTP_DELETE, p, h); }
void cttp_patch(cttp_server *s, const char *p, cttp_handler h)
{ route_add(s, CTTP_PATCH, p, h); }
void cttp_options(cttp_server *s, const char *p, cttp_handler h)
{ route_add(s, CTTP_OPTIONS, p, h); }

void cttp_use(cttp_server *s, cttp_middleware mw)
{
    if (s->nmw < CTTP_MAX_MIDDLEWARE)
        s->mw[s->nmw++] = mw;
}

void cttp_on_error(cttp_server *s,
                   void (*h)(cttp_request *, cttp_response *))
{
    s->on_error = h;
}

void cttp_on_log(cttp_server *s,
                 void (*h)(const cttp_request *, int, size_t))
{
    s->on_log = h;
}

/* ---- the event loop ------------------------------------------------------ */

void cttp_listen(cttp_server *s)
{
    g_server = s;
    s->running = 1;
    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa = { .sa_handler = handle_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* socket() -> setsockopt() -> bind() -> listen(); the listener is
     * also non-blocking so the accept loop below spins until EAGAIN.  */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)s->port);
    if (inet_pton(AF_INET, s->host, &addr.sin_addr) != 1) {
        cttp_log_error("invalid address: %s", s->host);
        close(fd);
        return;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(fd);
        return;
    }
    if (listen(fd, LISTEN_BACKLOG) < 0) {
        perror("listen");
        close(fd);
        return;
    }
    set_nonblock(fd);

    table_reserve(s, 1);
    s->listen_fd = fd;
    s->pfds[0]   = (struct pollfd){ .fd = fd, .events = POLLIN };
    s->conns[0]  = NULL;                 /* slot 0 = listener      */
    s->nconns    = 1;

    cttp_log_info("listening on http://%s:%d (webroot: %s)",
                  s->host, s->port, s->webroot ? s->webroot : "(none)");

    while (s->running) {
        /* RENEW INTEREST: poll()'s .events are ours to maintain —
         * pending response bytes => also ask for POLLOUT, else POLLIN. */
        for (int i = 1; i < s->nconns; i++)
            s->pfds[i].events = POLLIN |
                ((s->conns[i]->out_off < s->conns[i]->out.len)
                     ? POLLOUT : 0);

        int rc = poll(s->pfds, (nfds_t)s->nconns, POLL_INTERVAL_MS);
        if (rc < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        if (s->pfds[0].revents & POLLIN)
            accept_new_clients(s);
        s->pfds[0].revents = 0;

        for (int i = 1; i < s->nconns; ) {
            conn *c = s->conns[i];
            short rev = s->pfds[i].revents;   /* slot may move on close */
            s->pfds[i].revents = 0;
            int handled = 1;

            if (rev & (POLLERR | POLLHUP | POLLNVAL)) {
                conn_close(s, i);
                handled = 0;
            }
            else if (rev & POLLIN) {
                char tmp[IO_CHUNK];
                for (;;) {
                    ssize_t n = recv(c->fd, tmp, sizeof tmp, 0);
                    if (n > 0) {
                        buf_append(&c->in, tmp, (size_t)n);
                        c->last_activity = time(NULL);
                        if (n < (ssize_t)sizeof tmp) break;
                        continue;
                    }
                    if (n == 0) { conn_close(s, i); handled = 0; break; }
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    conn_close(s, i); handled = 0; break;
                }
                if (handled && http_read_step(c, s) < 0) {
                    conn_close(s, i); handled = 0;
                }
            }
            else if (rev & POLLOUT) {
                int r = flush_conn(c);
                if (r < 0) { conn_close(s, i); handled = 0; }
                else if (r == 1) on_flush_complete(s, i);
            }

            if (handled) i++;
        }

        /* reap idle connections (simple DoS hygiene) */
        time_t now = time(NULL);
        for (int i = s->nconns - 1; i >= 1; i--)
            if (now - s->conns[i]->last_activity > s->timeout_secs)
                conn_close(s, i);
    }

    cttp_log_info("shutting down");
}

void cttp_free(cttp_server *s)
{
    close(s->listen_fd);
    free(s->pfds);
    free(s->conns);
    s->pfds = NULL;
    s->conns = NULL;
    s->nconns = s->cap = 0;
}

#endif /* CTTP_IMPLEMENTATION */
