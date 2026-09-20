/* ==========================================================================
 * main.c — configuration, demo routes, wiring.
 * ========================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cttp.h"

/* ---- demo route handlers -------------------------------------------------
 * A handler receives the parsed request and must fill in the response.
 * Nothing here blocks: real servers would use a thread pool for slow
 * work (DB queries, templates) — see README "going further".           */

static void h_hello(http_request *req, http_response *res)
{
    http_res_json(res, 200, "{\"hello\":\"world\",\"server\":\"cttp\"}");
}

/* Route params were captured by router.c: /api/users/:id */
static void h_user(http_request *req, http_response *res)
{
    const char *id = req_param(req, "id");
    char json[256];
    snprintf(json, sizeof json, "{\"id\":\"%s\",\"name\":\"user %s\"}",
             id, id);
    http_res_json(res, 200, json);
}

/* Echo the request body back with the client's content-type. */
static void h_echo(http_request *req, http_response *res)
{
    const char *ct = req->get_header(req, "Content-Type");
    const char *mode = req_param(req, "mode");   /* /api/echo/:mode optional */
    if (mode && strcmp(mode, "upper") == 0) {
        buf_t out = {0};
        for (size_t i = 0; i < req->body.len; i++) {
            char c = req->body.data[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            buf_append(&out, &c, 1);
        }
        http_res_set(res, 200, ct ? ct : "text/plain",
                     out.data, out.len);
        buf_free(&out);
        return;
    }
    http_res_set(res, 200, ct ? ct : "text/plain",
                 req->body.data, req->body.len);
}

/* Streaming response: chunked transfer encoding exercised end-to-end. */
static void h_stream(http_request *req, http_response *res)
{
    for (int i = 1; i <= 5; i++)
        buf_printf(&res->body, "capped chunk %d\n", i);
    res->status = 200;
    snprintf(res->ctype, sizeof res->ctype, "text/plain");
    res->chunked = 1;                   /* http.c encodes chunks at send */
}

/* Small debug endpoint: shows every header the client sent. */
static void h_headers(http_request *req, http_response *res)
{
    buf_printf(&res->body, "{\"method\":\"%s\",\"path\":\"%s\","
                           "\"headers\":[", req->method_str, req->path);
    for (int i = 0; i < req->nheaders; i++)
        buf_printf(&res->body, "%s{\"name\":\"%s\",\"value\":\"%s\"}",
                   i ? "," : "", req->headers[i].name, req->headers[i].value);
    buf_append_str(&res->body, "]}");
    http_res_json(res, 200, res->body.data);
}

static void h_no_content(http_request *req, http_response *res)
{
    (void)req;
    res->status = 204;                  /* 204 must not carry a body */
    res->no_body = 1;
}

/* ---- wiring --------------------------------------------------------------- */

static void register_routes(server *s)
{
    server_route(s, HTTP_GET,     "/api/hello",    h_hello);
    server_route(s, HTTP_GET,     "/api/users/:id", h_user);
    server_route(s, HTTP_GET,     "/api/headers",  h_headers);
    server_route(s, HTTP_GET,     "/api/stream",   h_stream);
    server_route(s, HTTP_DELETE,  "/api/echo",     h_no_content);
    server_route(s, HTTP_POST,    "/api/echo",     h_echo);
    server_route(s, HTTP_POST,    "/api/echo/:mode", h_echo);
    server_route(s, HTTP_PUT,     "/api/echo",     h_echo);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "cttp — a tiny educational HTTP/1.1 server\n"
        "usage: %s [-p port] [-a address] [-r webroot] [-t timeout]\n",
        argv0);
}

int main(int argc, char **argv)
{
    int port = 8080;
    const char *host = "127.0.0.1";
    const char *webroot = "public";
    int timeout_secs = 30;

    int opt;
    while ((opt = getopt(argc, argv, "p:a:r:t:h")) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 'a': host = optarg; break;
        case 'r': webroot = optarg; break;
        case 't': timeout_secs = atoi(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    server s;
    if (server_init(&s, host, port, webroot) != 0)
        return 1;
    s.timeout_secs = timeout_secs;
    register_routes(&s);

    server_run(&s);
    server_free(&s);
    return 0;
}
