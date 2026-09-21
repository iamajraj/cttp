/* ==========================================================================
 * 01_hello.c — the smallest possible cttp server.
 *
 * Build:  make && ./build/01_hello
 * Try:    curl http://127.0.0.1:8081/
 * ========================================================================== */
#define CTTP_IMPLEMENTATION    /* must be defined before including the lib */
#include "cttp.h"

static void hello(http_request *req, http_response *res)
{
    (void)req;
    http_res_json(res, 200, "{\"message\":\"hello, world\"}");
}

int main(void)
{
    server s;
    if (server_init(&s, "127.0.0.1", 8081, NULL) != 0)
        return 1;

    server_route(&s, HTTP_GET, "/", hello);

    server_run(&s);            /* returns after Ctrl+C (SIGINT/SIGTERM) */
    server_free(&s);
}
