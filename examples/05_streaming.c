/* ==========================================================================
 * 05_streaming.c — chunked transfer-encoding responses and HEAD.
 *
 * A "live feed" of 5 lines uses Transfer-Encoding: chunked so the client
 * starts reading before the server knows the total length.
 *
 * Build:  make && ./build/05_streaming
 * Try:    curl -s http://127.0.0.1:8085/feed
 *         curl -I  http://127.0.0.1:8085/feed     (HEAD: headers only)
 * ========================================================================== */
#define CTTP_IMPLEMENTATION
#include "cttp.h"

static void feed(http_request *req, http_response *res)
{
    (void)req;
    for (int i = 1; i <= 5; i++)
        buf_printf(&res->body, "event %d: chunked by cttp\n", i);
    res->status = 200;
    snprintf(res->ctype, sizeof res->ctype, "text/plain");
    res->chunked = 1;     /* lib encodes chunks at send time */
}

int main(void)
{
    server s;
    if (server_init(&s, "127.0.0.1", 8085, NULL) != 0)
        return 1;

    server_route(&s, HTTP_GET, "/feed", feed);

    server_run(&s);
    server_free(&s);
    return 0;
}
