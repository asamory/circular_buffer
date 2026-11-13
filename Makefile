CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Iinclude
LDFLAGS ?= -pthread

all: build/cbuf_test build/log_demo

build/%: src/%.c include/cbuf.h
	@mkdir -p build
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

test: all
	./build/cbuf_test
	./build/log_demo 5 build/log.bin

clean:
	rm -f build/cbuf_test build/log_demo build/log.bin

.PHONY: all test clean
