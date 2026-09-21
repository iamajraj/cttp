/* ==========================================================================
 * 01_hello.c — the smallest possible cttp server.
 *
 * Build:  make && ./build/01_hello
 * Try:    curl http://127.0.0.1:8081/          (and watch the query param)
 * ========================================================================== */
#define CTTP_IMPLEMENTATION
#include "cttp.h"

static void hello(cttp_request *req, cttp_response *res)
{
    static int hits;                        /* one handler = shared state */
    cttp_json_begin(res);
    cttp_json_str(res, "message", "hello, world");
    cttp_json_int(res, "hits", ++hits);
    cttp_json_str(res, "who", cttp_query(req, "name")); /* NULL-safe */
    cttp_json_str(res, "req_id", cttp_req_id(req));
    cttp_json_end(res, 200);
}

int main(void)
{
    cttp_server srv;
    cttp_init(&srv);                     /* 127.0.0.1 + timeout defaults */
    srv.port = 8081;

    cttp_get(&srv, "/", hello);
    cttp_get(&srv, "/hello/:name", hello);      /* same handler, params */

    cttp_listen(&srv);                   /* blocks until Ctrl+C        */
    cttp_free(&srv);
}
