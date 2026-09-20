CC      = clang
CFLAGS  = -std=c11 -Wall -Wextra -Wno-unused-parameter -O2 -g
SRC     = src/buf.c src/log.c src/http.c src/router.c src/static.c src/server.c src/main.c
HDR     = src/cttp.h src/buf.h

cttp: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o cttp $(SRC)

clean:
	rm -f cttp

# curl test suite: run `make test` while the server runs on :8080
test: cttp
	./cttp -p 8080 &
	@sleep 0.3
	curl -s http://127.0.0.1:8080/api/hello
	curl -s http://127.0.0.1:8080/
	curl -s -X POST -d 'hi there' http://127.0.0.1:8080/api/echo
	curl -s -r 0-49 http://127.0.0.1:8080/ -o /dev/null -w 'range: %{http_code}\n'
	curl -s http://127.0.0.1:8080/api/stream
	kill %1 2>/dev/null || true

.PHONY: test clean
