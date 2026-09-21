# Examples

Each file is a complete, self-contained HTTP server program built against
the one-file library in `../include/cttp.h`. Build all of them from the
project root with `make`, or individually:

```sh
clang -Iinclude examples/01_hello.c -o /tmp/hello
```

| program | port | demonstrates |
|---|---|---|
| `01_hello.c` | 8081 | `cttp_init` + one route + `:params` + query |
| `02_rest_api.c` | 8082 | REST routes, `:params`, POST bodies, 201/204 |
| `03_static_site.c` | 8083 | file serving: MIME, ETag 304, Range 206 |
| `04_echo.c` | 8084 | request bodies, forms, cookies, headers, all methods |
| `05_streaming.c` | 8085 | chunked responses + SSE push (`curl -N`) |
| `06_notes_api.c` | 8086 | middleware, custom 404, redirect, wildcard, query |

Every example keeps a distinct port so several can run at once.
