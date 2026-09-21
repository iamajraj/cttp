CC     = clang
CFLAGS = -std=c11 -Wall -Wextra -Wno-unused-parameter -Iinclude -O2

EXAMPLES := $(wildcard examples/0*.c)
BINS := $(patsubst examples/%.c, build/%, $(EXAMPLES))

### build the example servers against include/cttp.h
build/% : examples/%.c include/cttp.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $<

all: $(BINS)

# Regenerate include/cttp.h from the annotated sources in internals/
gen:
	sh scripts/gen_lib.sh
	$(MAKE) -s all

clean:
	rm -rf build

# smoke-test example 02 (REST API on :8082)
test: build/02_rest_api
	./build/02_rest_api &
	@sleep 0.4
	curl -s http://127.0.0.1:8082/
	curl -s -X POST -d '{"name":"Ada"}' http://127.0.0.1:8082/users
	curl -s -X DELETE http://127.0.0.1:8082/users/1 -w '|%{http_code}\n'
	-kill %1 2>/dev/null

.PHONY: all gen clean test
