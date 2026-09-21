/* ==========================================================================
 * router.c — URL routing: ":params", wildcards, middleware, custom 404.
 *
 * A pattern like "/api/users/:id/files" + a wildcard segment, against the request path "/api/users/42/files/a.txt"
 * captures:  id = "42",  … = "a.txt" (wildcard = rest of the path).
 *
 * Matching walks pattern and path one '/'-segment at a time; ':' captures
 * one percent-decoded segment, '*' swallows the remainder.
 * ========================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cttp.h"

/* The middleware chain needs cttp_next() to find the server. The loop is
 * single-threaded, so one static pointer set right before dispatch is
 * safe and simple (larger frameworks thread a context through instead). */
static cttp_server *g_router;

/* Hand the request to the next middleware in the chain; when the chain
 * runs out, control returns to router_dispatch which runs the route.  */
static void mw_next(cttp_request *req, cttp_response *res)
{
    cttp_server *s = g_router;
    int i = req->mw_cur + 1;
    if (i < s->nmw) {
        req->mw_cur = i;
        s->mw[i](req, res, mw_next);
    }
}

/* Copy the next '/'-separated segment from *s, advancing the cursor and
 * reporting where the segment started. 1 = segment read, 0 = end.       */
static int next_seg(const char **s, const char **start,
                    char *out, size_t n)
{
    const char *p = *s;
    while (*p == '/') p++;
    *start = p;
    if (!*p) { out[0] = '\0'; *s = p; return 0; }
    const char *e = strchr(p, '/');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len >= n) len = n - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    *s = e ? e + 1 : p + len;
    return 1;
}

/* One route vs one request. 1 = match (captures set), 2 = path matches
 * but the method differs (drives a precise 405), 0 = no match.          */
static int route_match(int ridx, cttp_request *req)
{
    const char *p = g_router->routes[ridx].pattern;
    char pathcopy[1024];                    /* walk a scratch copy    */
    snprintf(pathcopy, sizeof pathcopy, "%s", req->path);
    const char *q = pathcopy, *qseg_start = pathcopy;

    char pseg[128], qseg[512];
    int np = 0;
    int result = 0;    /* 0 = no match, 1 = path + wildcard/direct hit */


    for (;;) {
        const char *pstart = p;
        int has_p = next_seg(&p, &pstart, pseg, sizeof pseg);
        int has_q = next_seg(&q, &qseg_start, qseg, sizeof qseg);

        if (!has_p && !has_q) { result = 1; break; }   /* both done */

        if (has_p != has_q) {                 /* one side ran out       */
            /* "/files/" + wildcard with nothing left: value = ""  */
            if (has_p && pseg[0] == '*' && np < CTTP_MAX_PARAMS) {
                cttp_request *rw = (cttp_request *)req;
                snprintf(rw->params[np], 64, "%s", pseg);
                rw->pvalues[np][0] = '\0';
                np++;
                result = 1;
            }
            break;
        }

        cttp_request *rw = (cttp_request *)req;
        if (pseg[0] == ':') {                /* one-segment capture    */
            if (np >= CTTP_MAX_PARAMS) break;
            snprintf(rw->params[np], 64, "%s", pseg + 1);
            if (cttp_url_decode(rw->pvalues[np], 256, qseg, 0) != 0)
                break;
            np++;
    } else if (pseg[0] == '*') {         /* swallow what's left    */
            if (np >= CTTP_MAX_PARAMS) break;
            snprintf(rw->params[np], 64, "%s", pseg);
            /* rest starts AT the current path segment (qseg_start) */
            if (cttp_url_decode(rw->pvalues[np], 256, qseg_start, 0) != 0)
                break;
            np++;
            result = 1;
            break;
        } else if (strcmp(pseg, qseg) != 0) {
            break;                           /* literal mismatch       */
        }
    }

    if (result != 0) {
        cttp_request *rw = (cttp_request *)req;
        /* commit captured params into the request */
        rw->nparams = np;
        /* HEAD is automatically served by GET handlers (the engine strips
     * the body when res.head_only is set) — RFC 9110 §9.3.2 behaviour. */
    cttp_method rm = g_router->routes[ridx].method;
    int ok = rm == req->method ||
             (req->method == CTTP_HEAD && rm == CTTP_GET);
    return ok ? 1 : 2;
    }
    return 0;
}

/* Middleware, then route, with sane fallbacks:
 *   OPTIONS without registration -> Allow: list (RFC 9110 §9.3.7)
 *   path exists, wrong method    -> 405 + Allow (always engine-made)
 *   GET/HEAD with a webroot      -> filesystem; missing file -> hook 404
 *   otherwise                    -> on_error hook or default JSON 404 */
void router_dispatch(cttp_server *s, cttp_request *req, cttp_response *res)
{
    g_router = s;
    req->mw_cur = -1;

    if (s->nmw > 0) {                     /* enter the chain         */
        req->mw_cur = 0;
        s->mw[0](req, res, mw_next);
    }

    if (res->responded)                   /* short-circuited: done   */
        return;

    int method_mismatch = 0;
    for (int i = 0; i < s->nroutes; i++) {
        int m = route_match(i, req);
        if (m == 1) {
            s->routes[i].handler(req, res);
            return;
        }
        if (m == 2) method_mismatch = 1;
    }

    if (req->method == CTTP_OPTIONS) {
        res->status = 200;
        cttp_set_header(res, "Allow", "GET, HEAD, POST, PUT, DELETE, OPTIONS");
        res->responded = 1;
        return;
    }

    if (method_mismatch) {                /* engine-owned, like OPTIONS */
        cttp_json_err(res, 405, "method not allowed");
        cttp_set_header(res, "Allow", "GET, HEAD, POST, PUT, DELETE, OPTIONS");
        res->responded = 1;
        return;
    }

    if ((req->method == CTTP_GET || req->method == CTTP_HEAD) && s->webroot) {
        static_serve(s, req, res);        /* filesystem fallback        */
        res->responded = 1;               /* static_serve always answers */
    }

    if (!res->responded) {
        if (s->on_error) s->on_error(req, res);
        else            cttp_json_err(res, 404, "resource not found");
        res->responded = 1;
    }
}
