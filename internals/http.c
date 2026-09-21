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

#include "cttp.h"

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
