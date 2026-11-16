CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Iinclude
LDFLAGS ?= -pthread

CXX      ?= c++
CXXFLAGS ?= -std=c++14 -O2 -Wall -Wextra -Wpedantic -Iinclude

# GoogleTest / GoogleMock (brew install googletest). gtest requires C++17.
GTEST_PREFIX ?= $(shell brew --prefix googletest 2>/dev/null || echo /usr/local)
GTEST_CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Iinclude -I$(GTEST_PREFIX)/include
GTEST_LDFLAGS  ?= -L$(GTEST_PREFIX)/lib -lgmock -lgtest -lgtest_main -pthread

C_TARGETS    := build/log_demo
CXX_TARGETS  := build/ring_pool_example
TEST_TARGETS := build/RingBufferTest build/buffer_pool_test

all: $(C_TARGETS) $(CXX_TARGETS) $(TEST_TARGETS)

build/%: src/%.c include/RingBuffer.h
	@mkdir -p build
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

build/%: src/%.cpp include/RingBufferPool.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@

build/%: tests/%.cpp include/RingBuffer.h include/RingBufferPool.hpp
	@mkdir -p build
	$(CXX) $(GTEST_CXXFLAGS) $< -o $@ $(GTEST_LDFLAGS)

build/buffer_pool_stress: tests/buffer_pool_stress.cpp include/RingBufferPool.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@

test: $(TEST_TARGETS) build/log_demo
	./build/RingBufferTest
	./build/buffer_pool_test
	./build/log_demo 5 build/log.bin

clean:
	rm -f $(C_TARGETS) $(CXX_TARGETS) $(TEST_TARGETS) build/buffer_pool_stress build/log.bin build/fault_capture_*.csv fault_capture_*.csv

.PHONY: all test clean
