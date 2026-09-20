/* ==========================================================================
 * router.c — URL routing with ":parameter" captures.
 *
 * A route pattern like "/api/users/:id/files/:name" matches the request
 * path "/api/users/42/files/report.txt" and captures:
 *      id   = "42"
 *      name = "report.txt"
 *
 * Matching is done by splitting both the pattern and the path into '/'
 * separated segments and comparing them one by one. A segment starting
 * with ':' matches anything and records the value.
 * ========================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cttp.h"

/* Value captured for parameter `name` in this request, or NULL. */
const char *req_param(const http_request *r, const char *name)
{
    for (int i = 0; i < r->nparams; i++)
        if (strcmp(r->params[i], name) == 0)
            return r->pvalues[i];
    return NULL;
}

/* Try to match one route pattern against the request path.
 * Return 1 on match (fills req->params), 0 on no match, 2 on path match
 * but wrong method (used to produce a precise 405). */
static int route_match(const route *rt, const http_request *r)
{
    char pat[256], path[1024];
    snprintf(pat, sizeof pat, "%s", rt->pattern);
    snprintf(path, sizeof path, "%s", r->path);

    http_request tmp;
    http_init_request(&tmp);

    char *psave = NULL, *rsave = NULL;
    char *pseg = strtok_r(pat, "/", &psave);
    char *rseg = strtok_r(path, "/", &rsave);
    int np = 0;

    while (pseg || rseg) {
        if (!pseg || !rseg) { http_free_request(&tmp); return 0; }
        if (pseg[0] == ':') {                       /* capture parameter */
            if (np >= CT_MAX_PARAMS) { http_free_request(&tmp); return 0; }
            snprintf(tmp.params[np],  64,               "%s", pseg + 1);
            snprintf(tmp.pvalues[np], CT_MAX_PARAM_STR, "%s", rseg);
            np++;
        } else if (strcmp(pseg, rseg) != 0) {
            http_free_request(&tmp); return 0;      /* literal mismatch */
        }
        pseg = strtok_r(NULL, "/", &psave);
        rseg = strtok_r(NULL, "/", &rsave);
    }

    /* path matched — copy captured params into a mutable view of the
     * request (we know it's really ours, so drop const deliberately) */
    http_request *rw = (http_request *)r;
    memcpy(rw->params,  tmp.params,  sizeof rw->params);
    memcpy(rw->pvalues, tmp.pvalues, sizeof rw->pvalues);
    rw->nparams = np;
    http_free_request(&tmp);

    return rt->method == r->method ? 1 : 2;
}

/* Find the best route and invoke it, or fall back sensibly:
 *   - no match at all        -> static file serving (webroot)
 *   - static can't serve it  -> 404 (with JSON body, like all errors)
 *   - path exists but method -> 405 Not Allowed with an Allow header
 */
void router_dispatch(server *s, http_request *req, http_response *res,
                     const char *realpath_used)
{
    (void)realpath_used;
    int method_mismatch = 0;

    for (int i = 0; i < s->nroutes; i++) {
        int m = route_match(&s->routes[i], req);
        if (m == 1) {
            s->routes[i].handler(req, res);
            return;
        }
        if (m == 2) method_mismatch = 1;
    }

    /* Built-in OPTIONS handler: the HTTP spec (RFC 9110 §9.3.7) says an
     * OPTIONS response should list the methods the server supports. */
    if (req->method == HTTP_OPTIONS) {
        res->status = 200;
        snprintf(res->extra_hdr, sizeof res->extra_hdr,
                 "Allow: GET, HEAD, POST, PUT, DELETE, OPTIONS\r\n");
        buf_append_str(&res->body, "");
        return;
    }

    if (method_mismatch) {
        http_res_error(res, 405, "method not allowed");
        snprintf(res->extra_hdr, sizeof res->extra_hdr,
                 "Allow: GET, HEAD, POST, PUT, DELETE, OPTIONS\r\n");
        return;
    }

    /* Fall back to static filesystem serving for GET/HEAD; everything
     * else becomes a JSON 404. */
    if (req->method == HTTP_GET || req->method == HTTP_HEAD) {
        static_serve(s, req, res);
        return;
    }

    http_res_error(res, 404, "resource not found");
}
