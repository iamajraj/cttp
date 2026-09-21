/* ==========================================================================
 * 04_echo.c — a body/query/cookie/headers playground in one route + crumbs.
 *
 * Build:  make && ./build/04_echo
 * Try:
 *   curl -X POST -d 'name=Ada&email=ada%40ex.com' http://127.0.0.1:8084/echo
 *   # form-decoding — no more manual url decoding
 *   curl -X PUT  -d 'hello cttp' http://127.0.0.1:8084/echo
 *   curl -b 'sid=abc123' -X POST -d 'x=1' http://127.0.0.1:8084/echo
 *   curl -c - -X POST -d 'y=2' http://127.0.0.1:8084/echo; echo;   (Set-Cookie)
 *   curl -X DELETE http://127.0.0.1:8084/echo                    (204)
 * ========================================================================== */
#define CTTP_IMPLEMENTATION
#include "cttp.h"

static void echo(cttp_request *req, cttp_response *res)
{
    /* hand back a session cookie when the client sent none */
    if (!cttp_cookie(req, "sid"))
        cttp_set_cookie(res, "sid", "abc123", &(cttp_cookie_opts){
            .max_age = 3600, .http_only = 1, .same_site = "Lax" });

    const char *ct = cttp_header(req, "Content-Type");
    if (cttp_streq_i(ct, "application/x-www-form-urlencoded")) {
        cttp_html(res, 200,
            "<h1>form</h1><p>name=%s</p><p>email=%s</p>\n",
            cttp_form(req, "name"), cttp_form(req, "email"));
        return;
    }

    cttp_json_begin(res);
    cttp_json_str(res, "content_type", ct);
    cttp_json_int(res, "length", (long long)req->body.len);
    cttp_json_str(res, "raw", req->body.data);
    cttp_json_str(res, "sid_cookie", cttp_cookie(req, "sid"));
    cttp_json_end(res, 200);
}

static void no_content(cttp_request *req, cttp_response *res)
{
    (void)req;
    cttp_no_content(res);
}

int main(void)
{
    cttp_server srv;
    cttp_init(&srv);
    srv.port = 8084;

    cttp_post(&srv,   "/echo", echo);
    cttp_put(&srv,    "/echo", echo);
    cttp_delete(&srv, "/echo", no_content);

    cttp_listen(&srv);
    cttp_free(&srv);
}
