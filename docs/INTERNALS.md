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
| 2 | `internals/buf.c`    | growable byte buffers |
| 3 | `internals/http.c`   | incremental parsing, bodies (Content-Length + chunked), 100-continue, response assembly |
| 4 | `internals/router.c` | pattern routes with `:params`, 405 + `Allow:`, OPTIONS |
| 5 | `internals/static.c` | MIME, ETag/304, byte ranges 206/416, traversal protection |
| 6 | `internals/server.c` | poll() loop, non-blocking I/O, idle timeout, signals |
| 7 | `internals/main.c`   | wiring everything together |

…then:
```sh
sh scripts/gen_lib.sh     # burn the updated sources into the one-file cttp.h
```

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
