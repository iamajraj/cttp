/* ==========================================================================
 * 04_echo.c — request inspection playground: bodies, headers, methods.
 *
 * Build:  make && ./build/04_echo
 * Try:
 *   curl -X POST -d 'hello cttp' http://127.0.0.1:8084/echo
 *   curl -X PUT  -d 'AaBb' http://127.0.0.1:8084/echo/upper
 *   curl -X DELETE http://127.0.0.1:8084/echo    (204)
 *   curl http://127.0.0.1:8084/inspect           (pretty header dump)
 * ========================================================================== */
#define CTTP_IMPLEMENTATION
#include "cttp.h"

static void echo(http_request *req, http_response *res)
{
    const char *ct = req->get_header(req, "Content-Type");
    buf_t out = {0};

    if (req_param(req, "mode") && strcmp(req_param(req, "mode"), "upper") == 0)
        for (size_t i = 0; i < req->body.len; i++) {
            char c = req->body.data[i];
            if (c >= 'a' && c <= 'z') c -= 32;      /* to upper-case */
            buf_append(&out, &c, 1);
        }
    else
        buf_append(&out, req->body.data, req->body.len);

    http_res_set(res, 200, ct ? ct : "text/plain", out.data, out.len);
    buf_free(&out);
}

static void no_content(http_request *req, http_response *res)
{
    (void)req;
    res->status = 204;
    res->no_body = 1;
}

static void inspect(http_request *req, http_response *res)
{
    buf_printf(&res->body, "method: %s\n", req->method_str);
    buf_printf(&res->body, "path:   %s\nquery: %s\n", req->path, req->query);
    buf_printf(&res->body, "HTTP/1.%d with %d headers:\n",
               req->version_minor, req->nheaders);
    for (int i = 0; i < req->nheaders; i++)
        buf_printf(&res->body, "  %s: %s\n",
                   req->headers[i].name, req->headers[i].value);
    buf_printf(&res->body, "body %zu bytes\n", req->body.len);

    http_res_set(res, 200, "text/plain; charset=utf-8",
                 res->body.data, res->body.len);
}

int main(void)
{
    server s;
    if (server_init(&s, "127.0.0.1", 8084, NULL) != 0)
        return 1;

    server_route(&s, HTTP_GET,    "/inspect",     inspect);
    server_route(&s, HTTP_DELETE, "/echo",        no_content);
    server_route(&s, HTTP_POST,   "/echo",        echo);
    server_route(&s, HTTP_POST,   "/echo/:mode",  echo);
    server_route(&s, HTTP_PUT,    "/echo",        echo);

    server_run(&s);
    server_free(&s);
    return 0;
}
