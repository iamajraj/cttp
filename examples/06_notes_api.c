/* ==========================================================================
 * 06_notes_api.c — everything together: middleware, JSON, cookies, query,
 * form data, redirects, CORS, wildcards and redirects.
 *
 * Build:  make && ./build/06_notes_api
 * Try:
 *   curl -c jar http://127.0.0.1:8086/login -d 'user=ada'             # cookie
 *   curl -b jar http://127.0.0.1:8086/notes?filter=a%20b               # query
 *   curl -b jar -X POST http://127.0.0.1:8086/notes -d 'text=hi+there'  # form
 *   curl http://127.0.0.1:8086/old-home -L                            # redirect
 *   curl -X OPTIONS http://127.0.0.1:8086/notes -D - | grep -i allow
 * ========================================================================== */
#define CTTP_IMPLEMENTATION
#include "cttp.h"

static void logger_mw(cttp_request *req, cttp_response *res, cttp_next next)
{
    if (!req->responded) {
        cttp_set_header(res, "X-Powered-By", "cttp/" CTTP_VERSION);
    }
    next(req, res);
}

static void auth_mw(cttp_request *req, cttp_response *res, cttp_next next)
{
    if (req->responded) { next(req, res); return; }   /* already answered */
    if (req->method == CTTP_GET &&
        cttp_streq_i(req->path, "/notes") &&
        !cttp_cookie(req, "session")) {
        cttp_json_err(res, 401, "missing session cookie (try /login first)");
        return;                                       /* short-circuit    */
    }
    next(req, res);
}

static void redirect_home(cttp_request *req, cttp_response *res)
{
    (void)req;
    cttp_redirect(res, 302, "/");
}

static void login(cttp_request *req, cttp_response *res)
{
    const char *user = cttp_form(req, "user");
    cttp_set_cookie(res, "session", user ? user : "anon",
                    &(cttp_cookie_opts){ .max_age = 300, .http_only = 1,
                                         .same_site = "Lax" });
    cttp_json_begin(res);
    cttp_json_str(res, "logged_in_as", user);
    cttp_json_end(res, 200);
}

static void list_notes(cttp_request *req, cttp_response *res)
{
    cttp_json_begin(res);
    cttp_json_str(res, "filter", cttp_query(req, "filter"));
    cttp_json_str(res, "user",   cttp_cookie(req, "session"));
    cttp_json_arr_begin(res, "notes");
    cttp_json_str(res, NULL, "buy milk");
    cttp_json_str(res, NULL, "learn c");
    cttp_json_arr_end(res);
    cttp_json_end(res, 200);
}

static void create_note(cttp_request *req, cttp_response *res)
{
    cttp_json_begin(res);
    cttp_json_str(res, "created", cttp_form(req, "text"));
    cttp_json_str(res, "user",    cttp_cookie(req, "session"));
    cttp_json_end(res, 201);
}

/* "wildcard": one handler serving /notes/:id/anything/else */
static void note_wildcard(cttp_request *req, cttp_response *res)
{
    cttp_json_begin(res);
    cttp_json_str(res, "id", cttp_param(req, "id"));
    cttp_json_str(res, "rest", cttp_param(req, "*"));
    cttp_json_end(res, 200);
}

/* custom 404 instead of the default JSON one */
static void not_found(cttp_request *req, cttp_response *res)
{
    cttp_html(res, 404,
        "<html><body><h1>404</h1><p>"
        "<code>%s %s</code> — are you lost?</p></body></html>\n",
        req->method_str, req->path);
}

int main(void)
{
    cttp_server srv;
    cttp_init(&srv);
    srv.port = 8086;

    cttp_use(&srv, logger_mw);
    cttp_use(&srv, auth_mw);
    cttp_on_error(&srv, not_found);

    cttp_get(&srv, "/", list_notes);
    cttp_get(&srv, "/login", login);
    cttp_post(&srv, "/login", login);
    cttp_get(&srv, "/notes", list_notes);
    cttp_post(&srv, "/notes", create_note);
    cttp_get(&srv, "/notes/:id/*", note_wildcard);
    cttp_get(&srv, "/old-home", redirect_home);

    cttp_listen(&srv);
    cttp_free(&srv);
}
