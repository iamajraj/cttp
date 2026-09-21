/* ==========================================================================
 * main.c — demo wiring: one of everything the library offers.
 * ========================================================================== */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cttp.h"

static void hello(cttp_request *req, cttp_response *res)
{
    (void)req;
    cttp_json_begin(res);
    cttp_json_str(res, "hello", "world");
    cttp_json_str(res, "quote", "say \"hi\"\nnewline\ttab");
    cttp_json_str(res, "server", "cttp/" CTTP_VERSION);
    cttp_json_end(res, 200);
}

/* :id parameter capture + query string access. */
static void get_user(cttp_request *req, cttp_response *res)
{
    cttp_json_begin(res);
    cttp_json_str(res, "id", cttp_param(req, "id"));
    cttp_json_str(res, "name", cttp_query(req, "name"));
    cttp_json_str(res, "req_id", cttp_req_id(req));
    cttp_json_end(res, 200);
}

/* Middleware: touches every response before the route runs. */
static void add_header_mw(cttp_request *req, cttp_response *res,
                          cttp_next next)
{
    (void)req;
    cttp_set_header(res, "X-Middleware", "ran-before-route");
    next(req, res);
}

/* POST/PUT echo of the raw body; DELETE => 204. */
static void echo(cttp_request *req, cttp_response *res)
{
    if (req->method == CTTP_DELETE) { cttp_no_content(res); return; }

    cttp_json_begin(res);
    cttp_json_str(res, "content_type", cttp_header(req, "Content-Type"));
    cttp_json_int(res, "length", (long long)req->body.len);
    cttp_json_str(res, "raw", req->body.data);
    cttp_json_str(res, "email", cttp_form(req, "email"));
    cttp_json_str(res, "sid", cttp_cookie(req, "sid"));
    cttp_json_end(res, 200);
}

/* JSON + escaping demo */
static void json_demo(cttp_request *req, cttp_response *res)
{
    (void)req;
    cttp_json_begin(res);
    cttp_json_str(res, "message", "line one\nsecond \"quoted\"\tvalue");
    cttp_json_int(res, "id", 42);
    cttp_json_bool(res, "ok", 1);
    cttp_json_null(res, "nothing");
    cttp_json_arr_begin(res, "tags");
    cttp_json_str(res, NULL, "c");
    cttp_json_str(res, NULL, "http");
    cttp_json_arr_end(res);
    cttp_json_obj_begin(res, "nested");
    cttp_json_str(res, "deep", "yes \"indeed\"");
    cttp_json_obj_end(res);
    cttp_json_end(res, 200);
}

/* SSE live feed. */
static void stream(cttp_request *req, cttp_response *res)
{
    (void)req;
    cttp_sse_start(res);
    for (int i = 1; i <= 5; i++) {
        char msg[64];
        snprintf(msg, sizeof msg, "tick %d", i);
        cttp_sse_send(res, "tick", msg);
    }
}

/* Demo logger hook: one line per request, right after handling. */
static void access_log(const cttp_request *req, int status, size_t bytes)
{
    cttp_log_info("%s %s -> %d (%zu bytes, id %s)",
                  req->method_str, req->path, status, bytes);
}

int main(int argc, char **argv)
{
    cttp_server s;
    cttp_init(&s);
    s.port = (argc > 1) ? atoi(argv[1]) : 8080;
    s.webroot = "public";

    cttp_use(&s, add_header_mw);
    cttp_on_log(&s, access_log);

    cttp_get(&s, "/", hello);
    cttp_get(&s, "/api/hello", hello);
    cttp_get(&s, "/api/users/:id", get_user);
    cttp_get(&s, "/api/json", json_demo);
    cttp_get(&s, "/api/stream", stream);
    cttp_post(&s, "/api/echo", echo);
    cttp_put(&s, "/api/echo", echo);
    cttp_delete(&s, "/api/echo", echo);

    cttp_listen(&s);
    cttp_free(&s);
    return 0;
}
