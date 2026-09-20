# cttp — a full-fledged HTTP/1.1 server written from scratch in C

A single-threaded, event-driven HTTP server (~1000 lines) built on nothing but
POSIX sockets — no threads, no epoll, no libraries. Every module carries
learning comments describing not just *what* it does but *why* real servers
do it that way.

## Build & run

```sh
make              # clang -std=c11 -Wall -Wextra
./cttp            # serves public/ on http://127.0.0.1:8080
./cttp -p 9000 -r ./public -a 0.0.0.0 -t 30
```

## Learning path (read in this order)

| # | File       | What you learn |
|---|------------|----------------|
| 1 | `src/cttp.h` | Data model: request, response, connection, route |
| 2 | `src/buf.c`  | Growable buffers — why sockets need them |
| 3 | `src/http.c` | Incremental parsing, Content-Length & chunked bodies, `Expect: 100-continue`, response assembly, HEAD/204/304 |
| 4 | `src/router.c` | Path routing with `:param` captures, 405 with `Allow:`, OPTIONS |
| 5 | `src/static.c` | MIME types, ETag/If-None-Match 304s, byte ranges (206/416), traversal protection |
| 6 | `src/server.c` | The `poll()` event loop, non-blocking I/O, graceful shutdown, idle reaping |
| 7 | `src/main.c` | Wiring: config parsing and demo handlers |

### The one diagram that explains everything

```
                 poll() event loop (single thread)
  accept ─┐        ┌─ CONN_READ_HEADERS ─ incremental parser
          ├─ conn ─┼─ CONN_READ_BODY / _CHUNK ─┐ parse → route → handler
          └        └─ CONN_WRITE (flush out)  ─┘        └─ keep-alive ↺
```

Each TCP connection is a small state machine living in `conn`. The event loop
never blocks: `read()`/`write()`/`accept()` return `EAGAIN` when the kernel
buffer is full/empty and we simply resume when `poll()` next reports the
socket is ready. This is how nginx-style servers get "one connection per
granted event" with almost no threads.

## Features

- HTTP/1.1, HTTP/1.0; keep-alive + pipelining tolerance
- Incremental request parsing with configurable header (16 KB) and body (10 MB) limits
- `Content-Length` bodies and **chunked** request decoding (RFC 9112 §7.1 state machine)
- `Expect: 100-continue` early acknowledgement (headers-only, then body)
- Methods GET, HEAD, POST, PUT, DELETE, OPTIONS, PATCH
- Router with `:param` captures, path-scoped 405s with `Allow:`, JSON errors
- Static files with ~20 MIME types, directory `index.html`, ETag + `If-None-Match`
  → 304, `Range: bytes=…` → 206 (resumable downloads / media seeking), 416 on bad ranges
- Streaming chunked **responses** (`GET /api/stream`)
- Malformed requests → 400/501/505; timeouts; SIGINT/SIGTERM graceful exit; SIGPIPE-safe writes
- Security guardrails: path traversal rejection, `TCP_NODELAY`, no unbounded buffers

## Try it

```sh
curl http://127.0.0.1:8080/                 # static index.html
curl http://127.0.0.1:8080/api/hello        # JSON
curl http://127.0.0.1:8080/api/users/42     # route params
curl -d 'hi' http://127.0.0.1:8080/api/echo # echo request body
curl http://127.0.0.1:8080/api/stream       # chunked streaming
curl -r 0-99 http://127.0.0.1:8080/         # byte range (206)
curl -I http://127.0.0.1:8080/                 # HEAD
printf "BAD\r\n\r\n" | nc 127.0.0.1 8080   # see a 400
```

## The two bugs you would have hit (we did)

1. **Blocking accept** — the listening socket must also be `O_NONBLOCK`, or
   the `accept()` loop stalls the entire server after the first client.
   (Found with `sample` on the hung process: it sat in `__accept`.)
2. **Static interest** — `poll()` events are read-modify-write: once a
   response is pending you must add `POLLOUT`, or the kernel will never wake
   you to flush. This is the event-loop contract most tutorials skip.

## Going further (good exercises)

- epoll/kqueue backends behind the same loop (Linux/BSD speedups)
- Thread pool + concurrent handlers for blocking work (DB, templating)
- `If-Modified-Since`, last-write wins; multipart/form parsing; TLS (handshake first
  at accept, then feed into the same parser); HTTP/1.1 101 Upgrade → WebSocket;
- slowloris mitigation via request-rate accounting; `sendfile()` zero-copy.
