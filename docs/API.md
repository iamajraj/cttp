# cttp API reference

Compile once: `#define CTTP_IMPLEMENTATION` then `#include "cttp.h"`.

## Types

| type | fields you use |
|---|---|
| `server` | opaque; pass to every server call |
| `http_request` | see below |
| `http_response` | fill this in inside handlers |

`http_request` fields:
```c
req->get_header(req, "Accept")        // NULL if missing
req->body      (buf_t: .data/.len)    // already decoded (length or chunked)
req_param(req, "id")                  // value for ":id" in the route
req->method, req->path, req->query
req->headers[i].name/.value           // raw header list, nheaders entries
```

`http_response`:
```c
res->status    default 200
res->chunked   stream as chunked transfer-encoding
res->no_body   for 204/304-style responses
res->extra_hdr extra raw header lines ("X-Foo: bar\r\n")
```

## Server lifecycle

| call | effect |
|---|---|
| `server_init(&s, host, port, webroot)` | bind + listen; `webroot` NULL disables static files; 0 on success |
| `server_route(&s, METHOD, "/users/:id", handler)` | register a route; errors become 405 w/ Allow |
| `server_run(&s)` | event loop, returns after SIGINT/SIGTERM |
| `server_free(&s)` | close sockets, free memory |

Routes are matched by `/`-segments; `:name` captures one value.
Unrouted GET/HEAD falls back to `webroot` static serving.

Note: all request bodies are capped (`CT_MAX_BODY_SZ`), headers too
(`CT_MAX_HEADER_SZ`); size worries surface as protocol errors (413).

Static responses include `ETag`,`Cache-Control: max-age=3600` — clients get
`304 Not Modified` when cached copies validate; `Range: bytes=` slices give
`206 Partial Content`.
