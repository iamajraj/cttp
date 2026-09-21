# cttp API reference

Compile once with `#define CTTP_IMPLEMENTATION` before
`#include "cttp.h"` (see README for the pattern).

## Types

| type | notes |
|---|---|
| `cttp_server` | opaque; set `port`, `webroot`, `timeout_secs` after `cttp_init` |
| `cttp_request` | use via helpers (`cttp_header`, `cttp_param`, ...) |
| `cttp_response` | fill the in with helpers; add JSON builder or stream |
| `cttp_method` | `CTTP_GET` `CTTP_HEAD` `CTTP_POST` `CTTP_PUT` `CTTP_DELETE` `CTTP_OPTIONS` `CTTP_PATCH` |
| `cttp_cookie_opts` | `{max_age, path, domain, same_site, http_only, secure}` — zero-fill for defaults |
| `cttp_middleware` | `void f(cttp_request*, cttp_response*, cttp_next)` |

## Server lifecycle

| call | effect |
|---|---|
| `cttp_init(&srv)` | set defaults (127.0.0.1:8080, timeout 30); adjust fields after |
| `cttp_get/post/put/delete/patch/head/options(&srv, path, handler)` | register route (also `cttp_route` with a cttp_method) |
| `cttp_use(&srv, mw)` | middleware: runs before handlers, in order |
| `cttp_on_error(&srv, handler)` | replaces default 404 with your handler |
| `cttp_on_log(&srv, fn)` | access-log hook, fires after handlers |
| `cttp_listen(&srv)` | bind + serve; returns after SIGINT/SIGTERM |
| `cttp_free(&srv)` | release sockets + memory |

### Routing

- exact match: `/api/hello`
- `:name` captures one segment: `/users/:id` → `cttp_param(req, "id")`
- `*` captures the remaining path, decoded: `/files/*`
- methods case-sensitive (as HTTP demands); unmatched paths become static
  files from `srv.webroot` when set, JSON 404 (`on_error` hook otherwise)
- built-in OPTIONS answers `Allow: ...`; 405s happen only when the path
  exists under another method

## Inside a handler

```c
const char *cttp_header(req, "Authorization");   // NULL if missing
const char *cttp_param (req, "id");              // route value
const char *cttp_query (req, "q");               // decoded query item
cttp_query_has(req, "q");                        // fast presence test
const char *cttp_form  (req, "email");           // form-urlencoded body
const char *cttp_cookie(req, "sid");             // parsed Cookie header
const char *cttp_req_id(req);                    // auto per-request id
```

## Response builders

All set the status + Content-Type + body for you and clear previous ones.

```c
cttp_text(res, 200, "you asked for %s", name);   // printf-style, escapes size itself
cttp_html(res, 200, "%s", html);
cttp_send (res, 200, "application/pdf", data, len);
cttp_redirect(res, 302, "/login");
cttp_no_content(res);
cttp_json_err(res, 404, "no such thing");
```

JSON builder — plain values, nesting, and arrays:

```c
cttp_json_begin(res);                            // "{" + application/json
cttp_json_str (res, "name", "Ada");              // \", \\ and control chars escaped
cttp_json_int (res, "year", 1815);
cttp_json_bool(res, "ok", 1);
cttp_json_arr_begin(res, "tags");
cttp_json_str (res, NULL, "c");                  // NULL key = array item
cttp_json_arr_end(res);
cttp_json_obj_begin(res, "nested");
cttp_json_str (res, "deep", "yes \"really\"");
cttp_json_obj_end(res);
cttp_json_end(res, 200);
```

## Cookies

```c
cttp_set_cookie(res, "sid", "abc", &(cttp_cookie_opts){
    .max_age = 3600, .http_only = 1, .same_site = "Lax" });
cttp_cookie(req, "sid");      // request side, parsed + cached
cttp_delete_cookie(res, "sid");
```

## Server side

```c
srv.port = 9000;
srv.webroot = "public";
srv.timeout_secs = 30;
```

Static files: `-srv.webroot`; `ETag: "size-mtime"` + `Cache-Control`
`Cache-Control: no-cache` for SSE; `If-None-Match` produces `304`. Byte
ranges (`Range: bytes=`) produce `206 Partial Content`, bad ranges `416`.
