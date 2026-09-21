# cttp internals — guided tour + next exercises

Exactly what the header `README` used to carry in detail is here. Entry
point list (read `#include` in the order listed).

## The one diagram that explains everything

```
                 poll() event loop (single thread)
  accept ─┐        ┌─ CONN_READ_HEADERS ─ incremental parser
          ├─ conn ─┼─ CONN_READ_BODY / _CHUNK ─┐ parse → route → handler
          └        └─ CONN_WRITE (flush out)  ─┘        └─ keep-alive ↺
```

Each TCP connection is a small state machine (`conn`). The loop never
blocks: `read()/write()/accept()` return `EAGAIN` when the kernel buffer
is full/empty and resume when `poll()` says the socket is ready. That's
how nginx-style servers handle thousands of connections without thousands
of threads.

## Reading order (fully commented sources)

| # | file | what you learn |
|---|------|----------------|
| 1 | `internals/cttp.h`   | data model: request / response / connection |
| 2 | `internals/buf.c`    | growable byte buffers + unbounded printf append |
| 3 | `internals/http.c`   | engine: decoding, bodies (CL + chunked), 100-continue, finalize |
| 4 | `internals/api.c`    | the helper layer: JSON builder, cookies, query/form, SSE |
| 5 | `internals/router.c` | `:params`, `*` wildcards, middleware chain, 405/OPTIONS |
| 6 | `internals/static.c` | MIME, ETag/304, byte ranges 206/416, traversal protection |
| 7 | `internals/server.c` | poll() loop, non-blocking I/O, idle timeout, signals |
| 8 | `internals/main.c`   | everything wired together |

…then:
```sh
sh scripts/gen_lib.sh     # burn the updated sources into include/cttp.h
```

## What v2 added to the engine (find each in the comment map above)

- **Percent-decoding** happens once per request in `parse_request_line`;
  the path is decoded BEFORE the `..` check, so `/%2e%2e/` traversals are
  caught by the same test that catches literal dots.
- **Response headers became a `buf_t`** (`res->headers`): any number of
  `cttp_set_header/cookie/cors` lines, CRLF-injection rejected by name.
- **SSE**: `cttp_sse_start` writes headers immediately, `cttp_sse_send`
  wraps each event in a chunk via the `stream_write` hook; when the
  handler returns, finalize only appends the terminating `0\r\n\r\n`
  (see `conn->streamed`).
- **Middleware**: dispatch runs `mw[0]`; each level resumes via
  `cttp_next` with a cursor on the request (`req->mw_cur`). Any
  responder sets `res.responded`, which short-circuits the chain.
- **X-Request-Id** generated per request (`Server-Request-Id` header);
  see `conn_reset_for_next`.

## Two bugs every event loop author hits

1. **Blocking accept** — the *listening* socket must be `O_NONBLOCK` too,
   or the `accept()` loop stalls the entire server after the first client.
   (Diagnosed with `sample ./cttp`: it sat in `__accept`.)
2. **Static poll interest** — `poll(fd).events` is read-modify-write.
   Once a response is pending one must raise `POLLOUT` or the kernel never
   wakes the loop to flush. This is the event loop contract tutorials skip.

## Going further (exercises)

- `kqueue`/`epoll` wrapper replacing poll (Linux perf)
- Thread pool + per-request queue for blocking work
- `If-Modified-Since` in addition to ETags
- TLS (handshake at accept; feed plaintext into the same parser)
- `101 Upgrade` → WebSocket handshake frames
- slowloris defence: request-body-rate accounting
- `sendfile()` zero-copy for large files, multipart/form parsing
