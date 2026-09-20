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

#include "cttp.h"

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
         * reflects what a GET would return, so we must not append body. */
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

    /* 2. Scratch copy so we can NUL-terminate lines in place. */
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
    c->next_state = c->state;      /* resume where we left off */
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
