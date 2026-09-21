/* ==========================================================================
 * 02_rest_api.c — smallest JSON REST API in cttp: :params, 201, 204.
 *
 * Build:  make && ./build/02_rest_api
 * Try:
 *   curl -X POST -d '{"name":"Ada"}' http://127.0.0.1:8082/users
 *   curl http://127.0.0.1:8082/users/1
 *   curl -X DELETE http://127.0.0.1:8082/users/1 -w '%{http_code}\n'
 * ========================================================================== */
#define CTTP_IMPLEMENTATION
#include "cttp.h"

static void health(cttp_request *req, cttp_response *res)
{
    (void)req;
    cttp_json_begin(res);
    cttp_json_str(res, "status", "ok");
    cttp_json_str(res, "version", CTTP_VERSION);
    cttp_json_end(res, 200);
}

static int next_id = 1;

static void create_user(cttp_request *req, cttp_response *res)
{
    int id = next_id++;
    cttp_json_begin(res);
    cttp_json_int(res, "id", id);
    cttp_json_str(res, "body", req->body.data);      /* escaped JSON-in */
    cttp_json_str(res, "name", cttp_form(req, "name"));
    cttp_json_end(res, 201);                          /* 201 Created */
}

static void get_user(cttp_request *req, cttp_response *res)
{
    cttp_json_begin(res);
    cttp_json_str(res, "id", cttp_param(req, "id"));
    cttp_json_str(res, "name", "demo");
    cttp_json_end(res, 200);
}

static void delete_user(cttp_request *req, cttp_response *res)
{
    (void)cttp_param(req, "id");
    cttp_no_content(res);                            /* 204, no body */
}

int main(void)
{
    cttp_server srv;
    cttp_init(&srv);
    srv.port = 8082;

    cttp_get(&srv, "/", health);
    cttp_post(&srv, "/users", create_user);
    cttp_get(&srv, "/users/:id", get_user);
    cttp_delete(&srv, "/users/:id", delete_user);

    cttp_listen(&srv);
    cttp_free(&srv);
}
