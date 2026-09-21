/* ==========================================================================
 * 05_streaming.c — chunked response + Server-Sent Events live feed.
 *
 * Build:  make && ./build/05_streaming
 * Try:
 *   curl -N http://127.0.0.1:8085/feed          (SSE push, one per line)
 *   curl -s http://127.0.0.1:8085/big | head -3 (plain chunked stream)
 * ========================================================================== */
#define CTTP_IMPLEMENTATION
#include "cttp.h"

/* A "log feed": the handler runs once per request; each cttp_sse_send
 * call flushes its own chunk, so the client sees events as they come.  */
static void feed(cttp_request *req, cttp_response *res)
{
    (void)req;
    cttp_sse_start(res);            /* headers leave immediately */
    for (int i = 1; i <= 3; i++) {
        char msg[64];
        snprintf(msg, sizeof msg, "tick %d at boot", i);
        cttp_sse_send(res, "tick", msg);
    }
    cttp_sse_send(res, "done", "that is all\nsecond line");
}

/* chunked without SSE: unknown length up-front */
static void big(cttp_request *req, cttp_response *res)
{
    (void)req;
    for (int i = 0; i < 5; i++)
        buf_printf(&res->body, "capped chunk %d of 5\n", i + 1);
    res->status = 200;
    snprintf(res->ctype, sizeof res->ctype, "text/plain");
    res->chunked = 1;
    res->responded = 1;
}

int main(void)
{
    cttp_server srv;
    cttp_init(&srv);
    srv.port = 8085;

    cttp_get(&srv, "/feed", feed);
    cttp_get(&srv, "/big",  big);

    cttp_listen(&srv);
    cttp_free(&srv);
}
