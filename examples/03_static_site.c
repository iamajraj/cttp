/* ==========================================================================
 * 03_static_site.c — turn cttp into a web server for a folder.
 *
 * Build:  make && ./build/03_static_site
 * Try:    curl -I http://127.0.0.1:8083/           (ETag, Content-Type)
 *         curl -r 0-99 http://127.0.0.1:8083/ -o /dev/null -w '%{http_code}\n'
 *         (then re-run with the ETag from -I to see a 304 Not Modified)
 * ========================================================================== */
#define CTTP_IMPLEMENTATION
#include "cttp.h"

int main(void)
{
    server s;
    /* webroot = "./public": everything below it becomes reachable,
     * ".." traversal attempts are rejected inside the lib.          */
    if (server_init(&s, "127.0.0.1", 8083, "public") != 0)
        return 1;

    /* No routes needed: unmatched GET/HEAD paths are served from webroot
     * automatically (index.html for "/"), MIME types included.          */

    server_run(&s);
    server_free(&s);
}
