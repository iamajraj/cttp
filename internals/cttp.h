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
