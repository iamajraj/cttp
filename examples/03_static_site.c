/* ==========================================================================
 * 03_static_site.c — cttp as a classic web server for a folder.
 *
 * Build:  make && ./build/03_static_site
 * Try:
 *   curl -I http://127.0.0.1:8083/            (ETag + Content-Type)
 *   curl -r 0-99 http://127.0.0.1:8083/ -o /dev/null -w '%{http_code}\n'
 *   curl http://127.0.0.1:8083/../nope         (traversal -> 403)
 * ========================================================================== */
#define CTTP_IMPLEMENTATION
#include "cttp.h"

int main(void)
{
    cttp_server srv;
    cttp_init(&srv);
    srv.port    = 8083;
    srv.webroot = "public";          /* everything under here is served */

    /* no routes needed: GET/HEAD paths fall through to the webroot.
     * "_done" is auto-served with MIME types, ETag/304, Range/206.     */

    cttp_listen(&srv);
    cttp_free(&srv);
}
