# cttp — build HTTP APIs in C with one file

Drop **`include/cttp.h`** into your project, `#define CTTP_IMPLEMENTATION`
once, write handlers, compile. No dependencies, no build step.

```c
#define CTTP_IMPLEMENTATION
#include "cttp.h"

static void user(cttp_request *req, cttp_response *res) {
    cttp_json_begin(res);
    cttp_json_str(res, "id", cttp_param(req, "id"));
    cttp_json_str(res, "name", cttp_query(req, "name"));
    cttp_json_end(res, 200);
}

int main(void) {
    cttp_server srv;
    cttp_init(&srv);                    /* defaults: 127.0.0.1:8080 */
    srv.webroot = "public";             /* optional static files    */
    cttp_get(&srv, "/api/users/:id", user);
    cttp_listen(&srv);                  /* blocks until Ctrl+C      */
    cttp_free(&srv);
}
```

```sh
clang -Iinclude your-server.c -o your-server     # that's the whole build
```

## Helper functions

| area | helpers |
|---|---|
| request | `cttp_header` `cttp_param` `cttp_query` `cttp_query_has` `cttp_form` `cttp_cookie` `cttp_req_id` |
| response | `cttp_send` `cttp_text` `cttp_html` (printf-style!) `cttp_no_content` `cttp_redirect` `cttp_set_header` `cttp_set_cookie`/`cttp_delete_cookie` `cttp_etag` `cttp_cache` `cttp_cors` `cttp_attachment` `cttp_json_err` |
| JSON | `cttp_json_begin` `cttp_json_str/int/double/bool/null/raw` `cttp_json_arr/obj_begin/end` `cttp_json_end` — full string escaping, no snprintf |
| SSE | `cttp_sse_start` `cttp_sse_send` — real push streaming |
| utilities | `cttp_url_decode` `cttp_url_encode` `cttp_trim` `cttp_streq_i` `cttp_http_date` `cttp_parse_http_date` `cttp_status_text` |

Carbon features in the engine: percent-decoded paths/queries/params,
wildcard route segments (`/files/*`), middleware chain (`cttp_use`),
custom 404 (`cttp_on_error`), access logs (`cttp_on_log`), automatic
X-Request-Id, keep-alive + pipelining, chunked TE both ways,
`Expect: 100-continue`, ETag 304s, byte ranges (206/416), 413/400/405/501
handling, graceful shutdown, cookie/form/query parsing.

## Examples

| example | what it demonstrates | port |
|---|---|---|
| `examples/01_hello.c` | minimal server, params, query | 8081 |
| `examples/02_rest_api.c` | REST routes, 201/204, JSON escape | 8082 |
| `examples/03_static_site.c` | folder serving: MIME, ETag 304, Range 206 | 8083 |
| `examples/04_echo.c` | all request readers: body, form, cookies, headers | 8084 |
| `examples/05_streaming.c` | chunked + Server-Sent Events | 8085 |
| `examples/06_notes_api.c` | middleware, cookies, query, redirect, wildcard, custom 404 | 8086 |

```sh
make            # builds every example into build/
./build/06_notes_api &
```

## Next steps / exercises

gzip, multipart/form, Basic auth + base64, TLS, WebSockets, epoll/kqueue —
`docs/INTERNALS.md` walks all of them.

Compile once: `#define CTTP_IMPLEMENTATION` above `#include "cttp.h"`.
