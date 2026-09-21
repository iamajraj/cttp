# cttp — build HTTP APIs in C with one file

Drop **`include/cttp.h`** into your project, write your handlers, compile. That's the
whole deal (stb-style header: the same file is the declarations *and* the
implementation — `#define CTTP_IMPLEMENTATION` once to compile the server code in).

```c
#define CTTP_IMPLEMENTATION
#include "cttp.h"

static void user(http_request *req, http_response *res) {
    char json[128];
    snprintf(json, sizeof json, "{\"id\":\"%s\"}", req_param(req, "id"));
    http_res_json(res, 200, json);
}

int main(void) {
    server s;
    server_init(&s, "127.0.0.1", 8080, NULL);          /* NULL => no static dir */
    server_route(&s, HTTP_GET, "/users/:id", user);
    server_run(&s);        /* until SIGINT/SIGTERM */
    server_free(&s);
}
```

```sh
clang -Iinclude your-server.c -o your-server     # that's it, no other dependencies
```

## Examples

| example | what it demonstrates | port |
|---|---|---|
| `examples/01_hello.c` | minimal server | 8081 |
| `examples/02_rest_api.c` | REST routes, `:params`, POST bodies, 201/204 | 8082 |
| `examples/03_static_site.c` | serving a folder (MIME, ETag 304, Range 206) | 8083 |
| `examples/04_echo.c` | req inspection: bodies, headers, all methods | 8084 |
| `examples/05_streaming.c` | chunked transfer-encoding responses | 8085 |

```sh
make            # builds every example into build/
./build/02_rest_api &
curl -d '{"name":"Ada"}' http://127.0.0.1:8082/users
```

## API surface

Types: `server`, `http_request`, `http_response`, `http_method`

```c
int  server_init(server*, const char *host, int port, const char *webroot);
int  server_route(server*, http_method, const char *pattern, http_handler);
void server_run(server*);          /* event loop */
void server_free(server*);

/* inside handlers */
void http_res_json(http_response*, int status, const char *json);
void http_res_text(http_response*, int status, const char *text);
void http_res_set (http_response*, int status, const char *ctype,
                   const void *body, size_t len);
const char *req_param(const http_request*, const char *name);
req->get_header(req, "Content-Type");   /* helper on every request */
```

See `docs/API.md` for the full list, `docs/INTERNALS.md` — a guided tour of
the event loop, parsing, chunked encoding and static-file caching — with the
fully commented sources in **`internals/`**.

## Status / limits (by design)

HTTP/1.1 (+1.0), keep-alive, incremental parsing, chunked bodies in and out,
`Expect: 100-continue`, byte ranges, ETags, 16 KB header / 10 MB body caps,
SIGINT/SIGTERM graceful shutdown. No TLS, no threads — the "next steps"
section of `docs/INTERNALS.md` covers those exercises.
