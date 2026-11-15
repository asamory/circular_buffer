CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Iinclude
LDFLAGS ?= -pthread

CXX      ?= c++
CXXFLAGS ?= -std=c++14 -O2 -Wall -Wextra -Wpedantic -Iinclude

all: build/cbuf_test build/log_demo build/ring_pool_test

build/%: src/%.c include/cbuf.h
	@mkdir -p build
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

build/ring_pool_test: src/ring_pool_test.cpp include/ring_pool.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@

test: all
	./build/cbuf_test
	./build/log_demo 5 build/log.bin
	./build/ring_pool_test

clean:
	rm -f build/cbuf_test build/log_demo build/log.bin build/ring_pool_test

.PHONY: all test clean
