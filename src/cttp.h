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

#include "buf.h"

/* ---- tunables ---------------------------------------------------------- */
#define CT_MAX_HEADERS    32        /* max headers per request            */
#define CT_MAX_HEADER_SZ  (16*1024) /* cap on the whole header block     */
#define CT_MAX_BODY_SZ    (10*1024*1024) /* cap on request body (10 MB)  */
#define CT_MAX_PARAMS     8         /* :name captures per route           */
#define CT_MAX_ROUTES     64
#define CT_MAX_PARAM_STR  128

/* ---- HTTP methods we understand ---------------------------------------- */
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
    int        want_continue;     /* we still owe the client a 100      */
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
