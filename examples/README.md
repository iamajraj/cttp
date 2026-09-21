# Examples

Each file is a complete, self-contained HTTP server program built against
the one-file library in `../include/cttp.h`. Build all of them with
`make` from the project root and run them from `build/`:

| program | port | demonstrates |
|---|---|---|
| `01_hello.c` | 8081 | `server_init` + one route |
| `02_rest_api.c` | 8082 | route params, POST bodies, 201/204 |
| `03_static_site.c` | 8083 | file serving: MIME, ETag 304, Range 206 |
| `04_echo.c` | 8084 | request bodies, headers, all methods |
| `05_streaming.c` | 8085 | chunked transfer-encoding responses |

Every example keeps a distinct port so several can run at once.
