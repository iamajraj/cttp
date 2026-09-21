/* ==========================================================================
 * cttp.h — the single header that ties every module together.
 *
 * READING GUIDE (read the files in this order):
 *   1. cttp.h        -> data structures: request, response, connection.
 *   2. buf.c         -> a growable byte buffer (used everywhere).
 *   3. http.c        -> parsing requests, building responses.
 *   4. router.c      -> URL routing with :parameters.
 *   5. static.c      -> serving files: MIME, ETag, Range, 304.
 *   6. server.c      -> the poll() event loop (the heart of the server).
 *   7. main.c        -> configuration + the demo routes.
 *
 * BIG PICTURE: one process, one thread, non-blocking sockets, a poll()
 * loop. Each TCP connection is a tiny state machine:
 *
 *     READ headers -> READ body -> CALL HANDLER -> WRITE response
 *          ^                                                  |
 *          +__________________ keep-alive: loop ______________+
 * ========================================================================== */
#ifndef CTTP_H
#define CTTP_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* ==========================================================================
 * buf.h — growable byte buffer.
 * ========================================================================== */





typedef struct buf {
    char  *data;    /* malloc'd, always NUL-terminated after every append */
    size_t len;     /* bytes used (excludes the implicit NUL)             */
    size_t cap;     /* bytes allocated                                    */
} buf_t;

void buf_reserve(buf_t *b, size_t need);
void buf_append(buf_t *b, const void *p, size_t n);
void buf_append_str(buf_t *b, const char *s);
void buf_printf(buf_t *b, const char *fmt, ...);
void buf_consume(buf_t *b, size_t n);   /* drop n leading bytes */
void buf_free(buf_t *b);

/* ---- tunables ---------------------------------------------------------- */
#define CT_MAX_HEADERS    32        /* max headers per request            */
#define CT_MAX_HEADER_SZ  (16*1024) /* cap on the whole header block     */
#define CT_MAX_BODY_SZ    (10*1024*1024) /* cap on request body (10 MB)  */
#define CT_MAX_PARAMS     8         /* :name captures per route           */
#define CT_MAX_ROUTES     64
#define CT_MAX_PARAM_STR  128

/* ---- HTTP methods the server understands --------------------------------- */
typedef enum {
    HTTP_GET, HTTP_HEAD, HTTP_POST, HTTP_PUT,
    HTTP_DELETE, HTTP_OPTIONS, HTTP_PATCH, HTTP_UNKNOWN
} http_method;

/* ---- an HTTP request --------------------------------------------------- */
typedef struct http_request {
    http_method method;
    char        method_str[10];   /* as sent by the client, e.g. "GET"  */
    char        target[2048];     /* request line target (raw)          */
    char        path[1024];       /* target with query string stripped  */
    char        query[1024];      /* "?a=b" part, without the '?'       */
    int         version_minor;    /* 0 => HTTP/1.0, 1 => HTTP/1.1       */

    struct {                    /* headers stored as name/value pairs */
        char name[64];
        char value[768];
    } headers[CT_MAX_HEADERS];
    int nheaders;

    buf_t body;                   /* decoded body (Content-Length or    */
                                  /* chunked transfer decoding done)    */

    char params[CT_MAX_PARAMS][64];          /* names of :params        */
    char pvalues[CT_MAX_PARAMS][CT_MAX_PARAM_STR]; /* and captured values */
    int  nparams;

    /* helper: case-insensitive header lookup, NULL if missing */
    const char *(*get_header)(const struct http_request *, const char *name);
} http_request;

/* ---- an HTTP response -------------------------------------------------- */
typedef struct http_response {
    int   status;                 /* 200, 404, ...                      */
    char  ctype[64];              /* Content-Type header value          */
    buf_t body;                   /* response payload                   */
    int   chunked;                /* stream body using chunked TE       */
    int   head_only;              /* HEAD request: send headers only    */
    int   no_body;                /* 204/304: never send a body         */
    char  extra_hdr[256];         /* e.g. "Allow: GET, HEAD\r\n"        */
} http_response;

/* A handler fills in the response. Return value is ignored. */
typedef void (*http_handler)(http_request *req, http_response *res);

const char *req_param(const http_request *r, const char *name);

/* ---- routing ----------------------------------------------------------- */
typedef struct route {
    char         pattern[256];    /* e.g. "/api/users/:id"              */
    http_method  method;
    http_handler handler;
} route;

/* ---- one TCP connection ------------------------------------------------ */
typedef enum {
    CONN_READ_HEADERS,            /* accumulating the header block      */
    CONN_READ_BODY,               /* accumulating content-length body   */
    CONN_READ_CHUNK,              /* decoding chunked transfer encoding */
    CONN_WRITE,                   /* flushing response bytes out        */
    CONN_CLOSED
} conn_state;

typedef struct conn {
    int        fd;
    conn_state state;
    buf_t      in;                /* bytes read but not yet consumed    */
    buf_t      out;               /* response bytes not yet sent        */
    size_t     out_off;           /* index into out[] already sent      */
    time_t     last_activity;     /* for idle timeouts                  */

    http_request req;             /* request being built for this conn  */

    /* body accounting, used by the read-body state machine */
    long long  body_remaining;    /* for content-length bodies          */
    long long  chunk_rem;         /* bytes left in current chunk        */
    int        chunk_stage;       /* chunk state machine step           */
    int        expect_continue;   /* client sent Expect: 100-continue   */
    int        want_continue;     /* a 100 Continue is still owed      */
    int        keep_alive;        /* decided when response finalized    */
    conn_state next_state;        /* state to enter after flushing out  */
} conn;

/* ---- server ------------------------------------------------------------ */
typedef struct server {
    int         listen_fd;        /* the passive listening socket       */
    char        host[64];
    int         port;
    const char *webroot;          /* directory for static files         */

    struct pollfd *pfds;          /* parallel arrays: pfds[i] <-> conns[i] */
    conn        **conns;
    int nconns, cap;

    route routes[CT_MAX_ROUTES];
    int   nroutes;

    int timeout_secs;             /* idle connections are reaped after  */
    int running;                  /* cleared by SIGINT/SIGTERM          */
} server;

/* ---- public API (one prototype per module) ----------------------------- */
/* server.c */
int  server_init(server *s, const char *host, int port, const char *webroot);
int  server_route(server *s, http_method m, const char *pattern, http_handler h);
void server_run(server *s);
void server_free(server *s);
void server_shutdown(server *s);          /* signal-safe: sets running=0  */

/* http.c */
int  http_read_step(conn *c, server *s);  /* one state-machine step       */
void http_init_request(http_request *r);
void http_free_request(http_request *r);
void http_res_init(http_response *r);
void http_res_free(http_response *r);
void http_res_set(http_response *r, int status, const char *ctype,
                  const void *body, size_t len);
void http_res_text(http_response *r, int status, const char *text);
void http_res_json(http_response *r, int status, const char *json);
void http_res_error(http_response *r, int status, const char *msg);
const char *http_status_text(int code);

/* router.c */
void router_dispatch(server *s, http_request *req, http_response *res,
                     const char *realpath_used);

/* static.c */
void static_serve(server *s, http_request *req, http_response *res);

/* log.c */
void log_info(const char *fmt, ...);
void log_error(const char *fmt, ...);

#endif /* CTTP_H */

/* ================= one-file IMPLEMENTATION ================= */
#ifdef CTTP_IMPLEMENTATION

#include <stdarg.h>
#include <sys/stat.h>
/* ==== from internals/buf.c ==== */
/* ==========================================================================
 * buf.c — a growable byte buffer.
 *
 * Why is this needed? Sockets deliver data in unpredictable chunk sizes,
 * and an HTTP message can arrive over many read() calls. We need somewhere
 * to accumulate bytes until a full message is present — that's this struct.
 * It is also used to build outgoing responses before writing them.
 * ========================================================================== */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>



/* Ensure capacity for at least 'need' more bytes, amortized doubling. */
void buf_reserve(buf_t *b, size_t need)
{
    if (b->len + need + 1 <= b->cap) return;
    size_t newcap = b->cap ? b->cap : 4096;
    while (newcap < b->len + need + 1) newcap *= 2;
    b->data = realloc(b->data, newcap);
    if (!b->data) { perror("realloc"); exit(1); }
    b->cap = newcap;
}

/* Append raw bytes. Always keeps data NUL-terminated for convenience,
 * although the NUL is NOT counted in ->len (binary-safe). */
void buf_append(buf_t *b, const void *p, size_t n)
{
    if (n == 0) return;
    buf_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void buf_append_str(buf_t *b, const char *s) { buf_append(b, s, strlen(s)); }

void buf_printf(buf_t *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) buf_append(b, tmp, (size_t)n);
}

/* Discard the first n bytes (memmove the rest to the front).
 * Used when we've finished parsing a message out of the read buffer and
 * want to keep any leftover bytes that belong to the next request. */
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
 * log.c — tiny timestamped logger.
 * ========================================================================== */
#include <stdarg.h>
#include <stdio.h>
#include <time.h>



static void vlog(const char *tag, const char *fmt, va_list ap)
{
    char ts[32];
    time_t now = time(NULL);
    struct tm tm;
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime_r(&now, &tm));
    fprintf(stderr, "[%s] %s ", ts, tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void log_info(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vlog("INFO", fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vlog("ERROR", fmt, ap);
    va_end(ap);
}


/* ==== from internals/http.c ==== */
/* ==========================================================================
 * http.c — HTTP/1.1 parsing and response building.
 *
 * The core protocol module. Per connection it drives a small state machine:
 *
 *   CONN_READ_HEADERS -> CONN_READ_BODY(/CHUNK) -> [router] -> CONN_WRITE
 *
 * Everything is incremental: a request may arrive in tiny TCP segments, so
 * parsing works on whatever bytes are currently in conn->in, consuming only
 * the complete parts and leaving the rest for later calls.
 * ========================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp, strncasecmp (POSIX) */
#include <time.h>
#include <errno.h>



/* Step results for the server loop. */
#define STEP_NEED_MORE    0
#define STEP_RESP_READY   1
#define STEP_FATAL       -1

/* ---- small helpers ------------------------------------------------------- */

static const char *req_get_header(const http_request *r, const char *name)
{
    for (int i = 0; i < r->nheaders; i++)
        if (strcasecmp(r->headers[i].name, name) == 0)
            return r->headers[i].value;
    return NULL;
}

/* Is 'token' present in the (case-insensitive) comma-separated list 'v'? */
static int has_token(const char *v, const char *token)
{
    size_t n = strlen(token);
    while (v && *v) {
        if (strncasecmp(v, token, n) == 0)
            return 1;
        const char *comma = strchr(v, ',');
        if (!comma) return 0;
        v = comma + 1;
        while (*v == ' ' || *v == '\t') v++;
    }
    return 0;
}

static http_method method_from_str(const char *s)
{
    if (!strcmp(s, "GET"))     return HTTP_GET;
    if (!strcmp(s, "HEAD"))    return HTTP_HEAD;
    if (!strcmp(s, "POST"))    return HTTP_POST;
    if (!strcmp(s, "PUT"))     return HTTP_PUT;
    if (!strcmp(s, "DELETE"))  return HTTP_DELETE;
    if (!strcmp(s, "OPTIONS")) return HTTP_OPTIONS;
    if (!strcmp(s, "PATCH"))   return HTTP_PATCH;
    return HTTP_UNKNOWN;
}

/* ---- request/response lifecycle ------------------------------------------ */

void http_init_request(http_request *r)
{
    memset(r, 0, sizeof *r);
    r->get_header = req_get_header;
}

void http_free_request(http_request *r) { buf_free(&r->body); }
void http_res_init(http_response *r)    { memset(r, 0, sizeof *r); r->status = 200; }
void http_res_free(http_response *r)    { buf_free(&r->body); }

const char *http_status_text(int code)
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
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 416: return "Range Not Satisfiable";
    case 417: return "Expectation Failed";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 505: return "HTTP Version Not Supported";
    default:  return "OK";
    }
}

/* ---- response builders (used by route handlers) --------------------------- */

void http_res_set(http_response *r, int status, const char *ctype,
                  const void *body, size_t len)
{
    r->status = status;
    snprintf(r->ctype, sizeof r->ctype, "%s", ctype ? ctype : "text/plain");
    buf_free(&r->body);
    buf_append(&r->body, body, len);
}

void http_res_text(http_response *r, int status, const char *text)
{
    http_res_set(r, status, "text/plain; charset=utf-8", text, strlen(text));
}

void http_res_json(http_response *r, int status, const char *json)
{
    http_res_set(r, status, "application/json", json, strlen(json));
}

void http_res_error(http_response *r, int status, const char *msg)
{
    buf_free(&r->body);
    r->status = status;
    snprintf(r->ctype, sizeof r->ctype, "application/json");
    buf_printf(&r->body, "{\"error\":{\"code\":%d,\"message\":\"%s\"}}",
               status, msg ? msg : http_status_text(status));
}

/* ---- building the response bytes ------------------------------------------ */

/* RFC 9110 §5.6.7 date: "Sun, 06 Nov 1994 08:49:37 GMT" */
static void http_gmt_date(char *out, size_t n, time_t t)
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

/* Decision rule for persistence (RFC 9112 §9.3):
 *   HTTP/1.1: persistent by default; close only if client asks.
 *   HTTP/1.0: close by default; persistent only if client asks. */
static void decide_keep_alive(conn *c)
{
    int keep = (c->req.version_minor >= 1);
    const char *h = req_get_header(&c->req, "Connection");
    if (h) {
        if (has_token(h, "close"))      keep = 0;
        if (has_token(h, "keep-alive")) keep = 1;
    }
    c->keep_alive = keep;
}

/* Format the finished response into conn->out and switch to CONN_WRITE.
 * Handles: status line, standard headers, chunked encoding for streaming
 * responses, HEAD (headers only) and no-body statuses (204/304/1xx). */
static void http_finalize_response(conn *c, server *s, http_response *res)
{
    buf_t *o = &c->out;
    char date[64];
    http_gmt_date(date, sizeof date, time(NULL));
    (void)s;

    decide_keep_alive(c);

    buf_printf(o, "HTTP/1.1 %d %s\r\n", res->status, http_status_text(res->status));
    buf_printf(o, "Server: cttp/1.0\r\n");
    buf_printf(o, "Date: %s\r\n", date);

    int no_body = res->no_body || res->status == 204 || res->status == 304 ||
                  res->status < 200;

    if (!no_body) {
        if (res->chunked) {
            /* Streaming: length unknown up-front -> chunked transfer
             * encoding (RFC 9112 §7.1). */
            buf_append_str(o, "Transfer-Encoding: chunked\r\n");
        } else {
            buf_printf(o, "Content-Type: %s\r\n",
                       res->ctype[0] ? res->ctype : "text/plain");
            buf_printf(o, "Content-Length: %zu\r\n", res->body.len);
        }
    }

    if (!c->keep_alive)
        buf_append_str(o, "Connection: close\r\n");

    if (res->extra_hdr[0])
        buf_append_str(o, res->extra_hdr);

    buf_append_str(o, "\r\n");                  /* blank line ends headers */

    if (no_body) {
        http_res_free(res);
        c->state = CONN_WRITE;
        return;
    }

    if (res->chunked) {
        /* Each chunk: "<hex length>\r\n<data>\r\n" ... then "0\r\n\r\n". */
        for (size_t i = 0; i < res->body.len; i += 4096) {
            size_t n = res->body.len - i > 4096 ? 4096 : res->body.len - i;
            buf_printf(o, "%zx\r\n", n);
            buf_append(o, res->body.data + i, n);
            buf_append_str(o, "\r\n");
        }
        buf_append_str(o, "0\r\n\r\n");
    } else if (!res->head_only) {
        /* HEAD: emit headers only — but Content-Length above already
         * reflects what a GET would return, so the body must not be appended. */
        buf_append(o, res->body.data, res->body.len);
    }

    http_res_free(res);
    c->state = CONN_WRITE;
}

/* ---- request-line and header parsing --------------------------------------- */

/* Split "GET /a/b?x=1 HTTP/1.1" into parts.
 * Return 0 ok, -1 malformed, -2 unsupported version. */
static int parse_request_line(char *line, http_request *r)
{
    char *sp1 = strchr(line, ' ');
    char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (!sp1 || !sp2) return -1;               /* need exactly 2 spaces */
    *sp1 = '\0';
    *sp2 = '\0';

    snprintf(r->method_str, sizeof r->method_str, "%s", line);
    r->method = method_from_str(line);
    snprintf(r->target, sizeof r->target, "%s", sp1 + 1);

    /* version: exactly "HTTP/1.0" or "HTTP/1.1" */
    const char *ver = sp2 + 1;
    if (strncmp(ver, "HTTP/1.", 7) != 0) return -2;
    if (ver[7] == '0' && ver[8] == '\0')      r->version_minor = 0;
    else if (ver[7] == '1' && ver[8] == '\0') r->version_minor = 1;
    else return -2;

    /* Split target into path and query. (Percent-decoding of the path is
     * deliberately left out — see README "exercises".) */
    const char *q = strchr(r->target, '?');
    if (q) {
        snprintf(r->path,  sizeof r->path,  "%.*s", (int)(q - r->target), r->target);
        snprintf(r->query, sizeof r->query, "%s", q + 1);
    } else {
        snprintf(r->path,  sizeof r->path,  "%s", r->target);
    }
    return 0;
}

/* Try to parse a complete header block from the front of c->in.
 * 1 = parsed, 0 = incomplete (need more bytes), -1 = malformed/flood. */
static int parse_headers(conn *c)
{
    buf_t *in = &c->in;
    http_request *r = &c->req;
    const char *data = in->data;
    size_t len = in->len;

    /* 1. Is the terminator "\r\n\r\n" present yet? */
    if (len < 4) return 0;
    const char *end = NULL;
    for (size_t i = 0; i + 3 < len; i++)
        if (memcmp(data + i, "\r\n\r\n", 4) == 0) { end = data + i; break; }
    if (!end) return len > CT_MAX_HEADER_SZ ? -1 : 0;

    size_t block_len = (size_t)(end - data) + 4;
    if (block_len > CT_MAX_HEADER_SZ) return -1;

    /* 2. Scratch copy: NUL-terminated lines are parsed in place. */
    char *block = malloc(block_len + 1);
    memcpy(block, data, block_len);
    block[block_len] = '\0';

    /* 3. Request line. */
    char *p = block, *cr = strchr(block, '\r');
    if (!cr || cr[1] != '\n') { free(block); return -1; }
    *cr = '\0';
    if (parse_request_line(p, r) != 0) { free(block); return -1; }
    p = cr + 2;

    /* 4. Header lines: "Name: value", until the empty CRLF line. */
    while (*p) {
        if (p[0] == '\r' && p[1] == '\n') break;   /* end of headers */
        char *nl = strstr(p, "\r\n");
        if (!nl) { free(block); return -1; }
        *nl = '\0';
        if (r->nheaders >= CT_MAX_HEADERS) { free(block); return -1; }
        char *colon = strchr(p, ':');
        if (!colon) { free(block); return -1; }
        *colon = '\0';
        const char *val = colon + 1;
        while (*val == ' ' || *val == '\t') val++;   /* strip leading OWS */
        snprintf(r->headers[r->nheaders].name,  64,  "%s", p);
        snprintf(r->headers[r->nheaders].value, 768, "%s", val);
        r->nheaders++;
        p = nl + 2;
    }

    buf_consume(in, block_len);            /* parsed bytes are consumed */
    free(block);
    return 1;
}

/* ---- body handling ---------------------------------------------------------- */

/* Interim response: both the 100 and the final response live in conn->out
 * in order; want_continue tells the flush logic to resume body reading. */
static void send_continue_100(conn *c)
{
    buf_append_str(&c->out, "HTTP/1.1 100 Continue\r\n\r\n");
    c->want_continue = 1;
    c->next_state = c->state;      /* resume where the parse left off */
    c->state = CONN_WRITE;
}

/* Content-Length body: copy bytes from conn->in into req.body until the
 * promised length is reached. 1 = complete, 0 = need more. */
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

/* Chunked transfer decoding (RFC 9112 §7.1), incremental state machine:
 *
 *   "1a;ext\r\n" + 26 data bytes + "\r\n"  ->  one chunk
 *   "0\r\n\r\n"                            ->  done
 *
 * chunk_stage: 0 = reading size line, 1 = inside data, 2 = trailer.
 * 1 = done, 0 = need more, negative = protocol error. */
static int read_body_chunked(conn *c)
{
    buf_t *in = &c->in;

    for (;;) {
        const char *data = in->data;
        size_t len = in->len;

        if (c->chunk_stage == 0) {                 /* size line */
            const char *nl = memchr(data, '\n', len);
            if (!nl) return len > 8192 ? -1 : 0;
            size_t line_len = (size_t)(nl - data) + 1;
            char line[64];
            if (line_len >= sizeof line) return -1;
            memcpy(line, data, line_len - 1);
            line[line_len - 1] = '\0';
            buf_consume(in, line_len);

            char *semi = strchr(line, ';');        /* ignore extensions */
            if (semi) *semi = '\0';
            unsigned long long sz = strtoull(line, NULL, 16);
            if (c->req.body.len + (size_t)sz > CT_MAX_BODY_SZ) return -2;
            c->chunk_rem = (long long)sz;
            c->chunk_stage = sz ? 1 : 2;
        }
        else if (c->chunk_stage == 1) {            /* chunk data */
            if (c->chunk_rem == 0) {               /* expect CRLF after data */
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
                if (c->chunk_rem == 0) c->chunk_stage = 1; /* wait for CRLF */
            }
        }
        else {                                     /* trailer section */
            if (len == 0) return 0;
            if (len >= 2 && data[0] == '\r' && data[1] == '\n') {
                buf_consume(in, 2);
                return 1;                          /* blank line: finished */
            }
            const char *nl = memchr(data, '\n', len);
            if (!nl) return len > 8192 ? -1 : 0;   /* skip trailer line */
            buf_consume(in, (size_t)(nl - data) + 1);
        }
    }
}

/* Determine body mode from headers and set conn->state.
 * 0 = reading body, 1 = no body (dispatch now), negative = error:
 *   -1 malformed, -2 body too large. */
static int begin_body(conn *c)
{
    http_request *r = &c->req;

    /* RFC 9112 §6.5: if Transfer-Encoding: chunked is present it wins. */
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
        if (n > CT_MAX_BODY_SZ) return -2;
        c->body_remaining = n;
        c->state = CONN_READ_BODY;
        return 0;                                  /* even n==0: completes
                                                      on next step */
    }
    return 1;                                      /* no body at all */
}

/* http_response lives on the heap because handlers embed it into bigger
 * document flows; the finalize step consumes it. */
static void dispatch_request(conn *c, server *s)
{
    http_response *res = malloc(sizeof *res);
    http_res_init(res);
    res->head_only = (c->req.method == HTTP_HEAD);

    router_dispatch(s, &c->req, res, NULL);
    http_finalize_response(c, s, res);
}

/* ---- explicit error response then close-or-respond --------------------------- */

static int error_response(conn *c, int status, const char *msg)
{
    http_response *res = malloc(sizeof *res);
    http_res_init(res);
    http_res_error(res, status, msg);
    http_finalize_response(c, NULL, res);
    return STEP_RESP_READY;
}

/* ---- the read state machine ----------------------------------------------------
 * Call after every read() that appended bytes (and right after accept).
 * Advances the connection as far as the available bytes allow.
 * Returns STEP_NEED_MORE / STEP_RESP_READY / STEP_FATAL. */
int http_read_step(conn *c, server *s)
{
    for (;;) {
        switch (c->state) {

        case CONN_READ_HEADERS: {
            int pr = parse_headers(c);
            if (pr == 0) return STEP_NEED_MORE;
            if (pr < 0)  return error_response(c, 400, "malformed request");

            http_request *r = &c->req;
            if (r->method == HTTP_UNKNOWN)
                return error_response(c, 501, "method not implemented");

            /* Decide the body mode BEFORE anything else: this both picks
             * the next state and lets an oversized Content-Length be
             * rejected with 413 without ever reading 10 MB of junk. */
            int b = begin_body(c);
            if (b < 0)
                return error_response(c, b == -2 ? 413 : 400,
                                      b == -2 ? "request body too large"
                                              : "bad content-length");

            if (b == 1) {
                /* No body: dispatch immediately. */
                dispatch_request(c, s);
                return STEP_RESP_READY;
            }

            /* RFC 9111 §10.1.1: with Expect: 100-continue acknowledge the
             * intent first ("yes, send your body"), then keep reading.
             * begin_body() already entered CONN_READ_BODY/CHUNK, so after
             * the interim flush resumes exactly at the right state. */
            const char *expect = req_get_header(r, "Expect");
            if (expect && strcasecmp(expect, "100-continue") == 0) {
                send_continue_100(c);
                return STEP_NEED_MORE;
            }
            continue;   /* move to body states */
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

        case CONN_WRITE:    /* between requests / mid-flush */
        case CONN_CLOSED:
            return STEP_NEED_MORE;
        }
    }
}


/* ==== from internals/router.c ==== */
/* ==========================================================================
 * router.c — URL routing with ":parameter" captures.
 *
 * A route pattern like "/api/users/:id/files/:name" matches the request
 * path "/api/users/42/files/report.txt" and captures:
 *      id   = "42"
 *      name = "report.txt"
 *
 * Matching is done by splitting both the pattern and the path into '/'
 * separated segments and comparing them one by one. A segment starting
 * with ':' matches anything and records the value.
 * ========================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>



/* Value captured for parameter `name` in this request, or NULL. */
const char *req_param(const http_request *r, const char *name)
{
    for (int i = 0; i < r->nparams; i++)
        if (strcmp(r->params[i], name) == 0)
            return r->pvalues[i];
    return NULL;
}

/* Try to match one route pattern against the request path.
 * Return 1 on match (fills req->params), 0 on no match, 2 on path match
 * but wrong method (used to produce a precise 405). */
static int route_match(const route *rt, const http_request *r)
{
    char pat[256], path[1024];
    snprintf(pat, sizeof pat, "%s", rt->pattern);
    snprintf(path, sizeof path, "%s", r->path);

    http_request tmp;
    http_init_request(&tmp);

    char *psave = NULL, *rsave = NULL;
    char *pseg = strtok_r(pat, "/", &psave);
    char *rseg = strtok_r(path, "/", &rsave);
    int np = 0;

    while (pseg || rseg) {
        if (!pseg || !rseg) { http_free_request(&tmp); return 0; }
        if (pseg[0] == ':') {                       /* capture parameter */
            if (np >= CT_MAX_PARAMS) { http_free_request(&tmp); return 0; }
            snprintf(tmp.params[np],  64,               "%s", pseg + 1);
            snprintf(tmp.pvalues[np], CT_MAX_PARAM_STR, "%s", rseg);
            np++;
        } else if (strcmp(pseg, rseg) != 0) {
            http_free_request(&tmp); return 0;      /* literal mismatch */
        }
        pseg = strtok_r(NULL, "/", &psave);
        rseg = strtok_r(NULL, "/", &rsave);
    }

    /* path matched — copy captured params into a mutable view of the
     * request: the comment says what happened — drop const deliberately */
    http_request *rw = (http_request *)r;
    memcpy(rw->params,  tmp.params,  sizeof rw->params);
    memcpy(rw->pvalues, tmp.pvalues, sizeof rw->pvalues);
    rw->nparams = np;
    http_free_request(&tmp);

    return rt->method == r->method ? 1 : 2;
}

/* Find the best route and invoke it, or fall back sensibly:
 *   - no match at all        -> static file serving (webroot)
 *   - static can't serve it  -> 404 (with JSON body, like all errors)
 *   - path exists but method -> 405 Not Allowed with an Allow header
 */
void router_dispatch(server *s, http_request *req, http_response *res,
                     const char *realpath_used)
{
    (void)realpath_used;
    int method_mismatch = 0;

    for (int i = 0; i < s->nroutes; i++) {
        int m = route_match(&s->routes[i], req);
        if (m == 1) {
            s->routes[i].handler(req, res);
            return;
        }
        if (m == 2) method_mismatch = 1;
    }

    /* Built-in OPTIONS handler: the HTTP spec (RFC 9110 §9.3.7) says an
     * OPTIONS response should list the methods the server supports. */
    if (req->method == HTTP_OPTIONS) {
        res->status = 200;
        snprintf(res->extra_hdr, sizeof res->extra_hdr,
                 "Allow: GET, HEAD, POST, PUT, DELETE, OPTIONS\r\n");
        buf_append_str(&res->body, "");
        return;
    }

    if (method_mismatch) {
        http_res_error(res, 405, "method not allowed");
        snprintf(res->extra_hdr, sizeof res->extra_hdr,
                 "Allow: GET, HEAD, POST, PUT, DELETE, OPTIONS\r\n");
        return;
    }

    /* Fall back to static filesystem serving for GET/HEAD; everything
     * else becomes a JSON 404. */
    if (req->method == HTTP_GET || req->method == HTTP_HEAD) {
        static_serve(s, req, res);
        return;
    }

    http_res_error(res, 404, "resource not found");
}


/* ==== from internals/static.c ==== */
/* ==========================================================================
 * static.c — serving files from the webroot.
 *
 * Modern static serving needs more than open()+read()+send():
 *   * path traversal protection  ("/../etc/passwd" must never escape)
 *   * MIME types by file extension
 *   * ETag + conditional requests (If-None-Match -> 304 Not Modified)
 *   * Range requests (resumable downloads, <video> seeking) -> 206
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

/* Reject traversal attempts. The path lives inside c->in, which is never
 * write raw to the socket, but it DOES end up in open() — so ".." and
 * NUL-ish shenanigans must be rejected before the filesystem call. */
static int is_unsafe_path(const char *p)
{
    if (!p || p[0] != '/') return 1;
    if (strstr(p, "..")) return 1;
    return 0;
}

/* Read the whole file referred to by `path` (already open, fd at 0).
 * Returns a heap buffer of exactly *out_len bytes, or NULL. */
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
        if (n == 0) break;               /* EOF before st_size? trust read */
        total += (size_t)n;
    }
    *out_len = total;
    data[total] = '\0';
    return data;
}

/* Serve fs paths under webroot for GET/HEAD. */
void static_serve(server *s, http_request *req, http_response *res)
{
    char path[2048];

    if (!s->webroot || is_unsafe_path(req->path)) {
        http_res_error(res, 403, "path not allowed");
        return;
    }

    /* Map the URL path onto the filesystem. Directory requests get
     * index.html appended (the classic "/path/" convention). */
    if (strcmp(req->path, "/") == 0)
        snprintf(path, sizeof path, "%s/index.html", s->webroot);
    else if (req->path[strlen(req->path) - 1] == '/')
        snprintf(path, sizeof path, "%s%sindex.html", s->webroot, req->path);
    else
        snprintf(path, sizeof path, "%s%s", s->webroot, req->path);

    int fd = open(path, O_RDONLY);
    if (fd < 0) { http_res_error(res, 404, "resource not found"); return; }
    struct stat st;
    fstat(fd, &st);
    if (S_ISDIR(st.st_mode)) {/* is a directory: retry with trailing-slash convention */
        close(fd);
        snprintf(path, sizeof path, "%s%s/index.html", s->webroot, req->path);
        fd = open(path, O_RDONLY);
        if (fd < 0) { http_res_error(res, 404, "resource not found"); return; }
        fstat(fd, &st);
    }

    /* ETag = size + mtime, hex. Cheap, effective for cache validation:
     * identical string means the browser's cached copy is still valid. */
    char etag[64];
    snprintf(etag, sizeof etag, "\"%zx-%zx\"",
             (size_t)st.st_size, (size_t)st.st_mtime);

    /* Conditional request: If-None-Match asks "is my cache still good?"  */
    const char *inm = req->get_header(req, "If-None-Match");
    if (inm && strstr(inm, etag)) {
        close(fd);
        res->status = 304;               /* 304 responses carry no body  */
        res->no_body = 1;
        snprintf(res->extra_hdr, sizeof res->extra_hdr,
                 "ETag: %s\r\nCache-Control: max-age=3600\r\n", etag);
        return;
    }

    /* Byte-range requests: if satisfiable, send exactly that slice. */
    const char *rh = req->get_header(req, "Range");
    if (rh) {
        unsigned long long start, end, total = (unsigned long long)st.st_size;
        if (sscanf(rh, "bytes=%llu-%llu", &start, &end) == 2 &&
            start < total && end >= start) {
            if (end >= total) end = total - 1;
        } else if (sscanf(rh, "bytes=%llu-", &start) == 1 && start < total) {
            end = total - 1;
        } else {
            /* Unsatisfiable: RFC 9110 §14.2 wants the real size back. */
            snprintf(res->extra_hdr, sizeof res->extra_hdr,
                     "Content-Range: bytes */%llu\r\n", total);
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
        snprintf(res->extra_hdr, sizeof res->extra_hdr,
                 "Content-Range: bytes %llu-%llu/%llu\r\n",
                 start, start + got - 1, total);
        buf_free(&res->body);
        buf_append(&res->body, data, got);
        free(data);
        return;
    }

    /* Plain full-file 200. */
    size_t len = 0;
    char *data = read_whole_file(fd, &len);
    close(fd);
    if (!data) { http_res_error(res, 500, "read failed"); return; }

    res->status = 200;
    snprintf(res->ctype, sizeof res->ctype, "%s", mime_for(path));
    snprintf(res->extra_hdr, sizeof res->extra_hdr,
             "ETag: %s\r\nCache-Control: max-age=3600\r\n", etag);
    buf_free(&res->body);
    buf_append(&res->body, data, len);
    free(data);
}


/* ==== from internals/server.c ==== */
/* ==========================================================================
 * server.c — sockets + the poll() event loop.
 *
 * ARCHITECTURE (the heart of the whole program)
 * ---------------------------------------------
 * One thread multiplexes every socket with poll(), using non-blocking I/O
 * (O_NONBLOCK). An fd is never allowed to block: when write/read would
 * block one simply stops and resume when poll() says the socket is ready.
 *
 * pfds[] and conns[] are parallel arrays: pfds[i] describes fd state for
 * conns[i]. pfds[0] is always the listening socket.
 *
 *   accept() --+--> conn state machine (see http.c)
 *              |
 *   idle sockets are reaped after server.timeout_secs seconds
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
#include <sys/time.h>



#define LISTEN_BACKLOG     128
#define IO_CHUNK           16384   /* bytes per read()/write() attempt   */
#define POLL_INTERVAL_MS   1000    /* wake up at least this often        */

/* Signal handlers can't get arguments, so the handler needs a way back to
 * the server struct: a file-scope pointer, written only at startup. */
static server *g_server;
static void handle_signal(int sig)
{
    (void)sig;
    /* The only "work" in a signal handler: set a volatile-sig_atomic_t
     * flag; the event loop checks it between iterations. For more
     * complex designs use the "self-pipe" trick. */
    if (g_server) g_server->running = 0;
}

/* ---- connection table -------------------------------------------------- */

static void table_reserve(server *s, int need)
{
    if (need <= s->cap) return;
    int newcap = s->cap ? s->cap : 16;
    while (newcap < need) newcap *= 2;
    s->pfds  = realloc(s->pfds,  (size_t)newcap * sizeof *s->pfds);
    s->conns = realloc(s->conns, (size_t)newcap * sizeof *s->conns);
    if (!s->pfds || !s->conns) { perror("realloc"); exit(1); }
    s->cap = newcap;
}

/* Set fd non-blocking: an O_NONBLOCK socket's read()/write() return
 * EAGAIN instead of stalling the whole server (classic blocking servers
 * used create one thread instead — a trade-off discussed in the README). */
static void set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void conn_reset_for_next(conn *c)
{
    http_free_request(&c->req);
    http_init_request(&c->req);
    c->out_off        = 0;
    c->body_remaining = 0;
    c->chunk_rem      = 0;
    c->chunk_stage    = 0;
    c->keep_alive     = 0;
    c->want_continue  = 0;
    c->state          = CONN_READ_HEADERS;
    buf_consume(&c->out, c->out.len);
    /* NOTE: c->in is NOT emptied — anything the client already sent for
     * the next request (pipelining) must survive for the next parse. */
}

static void conn_close(server *s, int idx)
{
    conn *c = s->conns[idx];
    close(c->fd);
    http_free_request(&c->req);
    buf_free(&c->in);
    buf_free(&c->out);
    free(c);

    /* Remove slot idx by moving the last entry into it (O(1), but it
     * changes iteration order — the loop tolerates re-checking slots). */
    s->conns[idx] = s->conns[s->nconns - 1];
    s->pfds[idx]  = s->pfds[s->nconns - 1];
    s->nconns--;
}

/* === setup & teardown ===================================================== */

/* Create the listening socket: the classic four-socket-API steps —
 * socket() -> setsockopt() -> bind() -> listen(). We keep it referenced
 * as pfds[0] in the poll array (see server_run). */
int server_init(server *s, const char *host, int port, const char *webroot)
{
    memset(s, 0, sizeof *s);
    table_reserve(s, 1);

    /* AF_INET + SOCK_STREAM = TCP over IPv4. */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    /* SO_REUSEADDR removes the TIME_WAIT wait after restarts, otherwise
     * a crash/restart loop would lose the port for up to 2*MSL (~2 min). */
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);              /* network byte order */
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid address: %s\n", host);
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, LISTEN_BACKLOG) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    /* Also make the LISTENING socket non-blocking: poll() then wakes us
     * only when a connection is *guaranteed* (no thundering-herd accept),
     * and the accept-for loop below can safely spin until EAGAIN. */
    set_nonblock(fd);

    s->listen_fd = fd;
    s->port = port;
    s->timeout_secs = 30;
    snprintf(s->host, sizeof s->host, "%s", host);
    s->webroot = webroot;
    s->pfds[0] = (struct pollfd){ .fd = fd, .events = POLLIN };
    s->conns[0] = NULL;                    /* slot 0 is the listener */
    s->nconns = 1;
    return 0;
}

int server_route(server *s, http_method m, const char *pattern, http_handler h)
{
    if (s->nroutes >= CT_MAX_ROUTES) return -1;
    snprintf(s->routes[s->nroutes].pattern, 256, "%s", pattern);
    s->routes[s->nroutes].method = m;
    s->routes[s->nroutes].handler = h;
    s->nroutes++;
    return 0;
}

void server_free(server *s)
{
    for (int i = s->nconns - 1; i >= 1; i--)
        conn_close(s, i);
    close(s->listen_fd);
    free(s->pfds);
    free(s->conns);
}

/* ---- accepting connections ---------------------------------------------- */

static void accept_new_clients(server *s)
{
    for (;;) {                                /* accept until EAGAIN:
                                                  multiple clients may be
                                                  queued at once */
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
        /* Nagle's algorithm batches small writes (40ms delay); an HTTP
         * server with small responses usually wants TCP_NODELAY. */
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        table_reserve(s, s->nconns + 1);
        conn *c = calloc(1, sizeof *c);
        c->fd = fd;
        c->state = CONN_READ_HEADERS;
        c->last_activity = time(NULL);
        http_init_request(&c->req);
        s->conns[s->nconns] = c;
        s->pfds[s->nconns]  = (struct pollfd){
            .fd = fd, .events = POLLIN, .revents = 0 };
        s->nconns++;
    }
}

/* ---- flushing outgoing bytes --------------------------------------------- */

/* Write as much of conn->out as the socket kernel buffer accepts right
 * now (non-blocking). Returns -1 on hard error (peer gone). */
static int flush_conn(conn *c)
{
    while (c->out_off < c->out.len) {
        ssize_t n = send(c->fd, c->out.data + c->out_off,
                         c->out.len - c->out_off, MSG_NOSIGNAL);
        if (n > 0) {
            c->out_off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;                       /* kernel buffer full; retry
                                               when poll says POLLOUT   */
        return -1;                          /* EPIPE and friends: gone  */
    }
    return 1;                               /* fully flushed */
}

static void on_flush_complete(server *s, int idx)
{
    conn *c = s->conns[idx];

    if (c->want_continue) {
        /* The "100 Continue" interim response just went out. Resume
         * exactly where the header parse left off (usually reading the
         * body the client has presumably already started sending). */
        c->want_continue = 0;
        c->state = c->next_state;
        c->out_off = 0;
        buf_consume(&c->out, c->out.len);
        return;                             /* next poll loop continues */
    }

    if (c->keep_alive) {
        conn_reset_for_next(c);             /* ready for next request (keep-alive) */
        return;
    }
    conn_close(s, idx);                     /* Connection: close */
}

/* ---- the event loop ------------------------------------------------------ */

void server_run(server *s)
{
    g_server = s;
    s->running = 1;

    /* SIGPIPE: writing to a socket the peer already closed would kill the
     * process by default. We ignore it and treat writes as errors instead. */
    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa = { .sa_handler = handle_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    log_info("listening on http://%s:%d (webroot: %s)",
             s->host, s->port, s->webroot ? s->webroot : "(none)");

    while (s->running) {
        /* --- RENEW INTEREST: recompute what each socket should listen for.
         * This is the event-loop contract most tutorials skip: poll()'s
         * .events are static, so the loop must update them every iteration to
         * reflect each connection's state machine —
         *   pending response bytes -> also want POLLOUT (writability),
         *   otherwise -> want POLLIN (new request bytes). */
        for (int i = 1; i < s->nconns; i++)
            s->pfds[i].events = POLLIN |
                ((s->conns[i]->out_off < s->conns[i]->out.len)
                     ? POLLOUT : 0);

        int rc = poll(s->pfds, (nfds_t)s->nconns, POLL_INTERVAL_MS);
        if (rc < 0) {
            if (errno == EINTR) continue;   /* signal interrupted us */
            perror("poll");
            break;
        }

        /* --- listening socket readable => pending client connections --- */
        if (s->pfds[0].revents & POLLIN)
            accept_new_clients(s);
        s->pfds[0].revents = 0;

        /* --- service each ready connection ---------------------------- */
        for (int i = 1; i < s->nconns; ) {
            conn *c = s->conns[i];
            short rev = s->pfds[i].revents; /* copy: slot may move on close */
            s->pfds[i].revents = 0;
            int handled = 1;                /* did this slot get consumed? */

            if (rev & (POLLERR | POLLHUP | POLLNVAL)) {
                conn_close(s, i);
                handled = 0;
            }
            else if ((rev & POLLIN)) {
                /* Caller wants new bytes. Drain until the socket would
                 * block — fewer poll() round trips per request. */
                char tmp[IO_CHUNK];
                for (;;) {
                    ssize_t n = recv(c->fd, tmp, sizeof tmp, 0);
                    if (n > 0) {
                        buf_append(&c->in, tmp, (size_t)n);
                        c->last_activity = time(NULL);
                        if (n < (ssize_t)sizeof tmp) break;   /* drained */
                        continue;
                    }
                    if (n == 0) {                 /* peer closed cleanly */
                        conn_close(s, i);
                        handled = 0;
                        break;
                    }
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    conn_close(s, i);             /* ECONNRESET etc     */
                    handled = 0;
                    break;
                }
                if (handled) {
                    c->last_activity = time(NULL);
                    if (http_read_step(c, s) < 0) {   /* STEP_FATAL */
                        conn_close(s, i);
                        handled = 0;
                    }
                }
            }
            else if (rev & POLLOUT) {
                int r = flush_conn(c);
                if (r < 0) { conn_close(s, i); handled = 0; }
                else if (r == 1) on_flush_complete(s, i);
                /* r == 0: kernel buffer full; retry on next POLLOUT */
            }

            if (handled) i++;
            /* if the slot was freed, the last entry moved into it — do
             * NOT advance: examine the replacement slot as well */
        }

        /* --- reap idle connections (simple DoS hygiene) ---------------- */
        time_t now = time(NULL);
        for (int i = s->nconns - 1; i >= 1; i--) {
            if (now - s->conns[i]->last_activity > s->timeout_secs)
                conn_close(s, i);
        }
    }

    log_info("shutting down");
}

#endif /* CTTP_IMPLEMENTATION */
