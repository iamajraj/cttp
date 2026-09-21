/* ==========================================================================
 * 02_rest_api.c — a small JSON REST API (:params, POST bodies, statuses).
 *
 * Build:  make && ./build/02_rest_api
 * Try:
 *   curl -X POST -d '{"name":"Ada"}' http://127.0.0.1:8082/users
 *   curl http://127.0.0.1:8082/users/1
 *   curl -X DELETE http://127.0.0.1:8082/users/1 -w '%{http_code}\n'
 * ========================================================================== */
#define CTTP_IMPLEMENTATION
#include "cttp.h"

static int next_id = 1;

static void health(http_request *req, http_response *res)
{
    (void)req;
    http_res_json(res, 200, "{\"status\":\"ok\"}");
}

static void create_user(http_request *req, http_response *res)
{
    int id = next_id++;
    char json[256];
    snprintf(json, sizeof json, "{\"id\":%d,\"body\":%.220s}", id,
             req->body.len ? req->body.data : "{}");
    http_res_json(res, 201, json);          /* 201 Created */
}

static void get_user(http_request *req, http_response *res)
{
    const char *id = req_param(req, "id");  /* captured from /users/:id */
    char json[256];
    snprintf(json, sizeof json, "{\"id\":\"%s\",\"name\":\"demo\"}", id);
    http_res_json(res, 200, json);
}

static void delete_user(http_request *req, http_response *res)
{
    (void)req_param(req, "id");
    res->status = 204;                      /* 204 No Content: no body */
    res->no_body = 1;
}

int main(void)
{
    server s;
    if (server_init(&s, "127.0.0.1", 8082, NULL) != 0)
        return 1;

    server_route(&s, HTTP_POST,   "/users",     create_user);
    server_route(&s, HTTP_GET,    "/users/:id", get_user);
    server_route(&s, HTTP_DELETE, "/users/:id", delete_user);
    server_route(&s, HTTP_GET,    "/",          health);

    server_run(&s);
    server_free(&s);
    return 0;
}
