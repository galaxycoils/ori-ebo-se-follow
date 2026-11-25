CXX ?= c++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic
PYTHON ?= python3
EMXX ?= em++
CORE = src/ori.cpp src/dock.cpp src/adapter.cpp
BUILD_ID := $(shell git describe --always --dirty 2>/dev/null || echo dev)

.PHONY: all test test-asan test-browser test-pages bench replay demo pages clean
all: build/ori-replay build/ori-probe

build:
	mkdir -p $@

build/ori-replay: $(CORE) src/ori.hpp src/replay.cpp | build
	$(CXX) $(CXXFLAGS) $(CORE) src/replay.cpp -o $@

build/ori-probe: src/ori.cpp src/dock.cpp src/ori.hpp src/probe.cpp | build
	$(CXX) $(CXXFLAGS) -DORI_BUILD_ID=\"$(BUILD_ID)\" src/ori.cpp src/dock.cpp src/probe.cpp -o $@

build/ori-tests: $(CORE) src/ori.hpp src/adapter.hpp tests/core.cpp | build
	$(CXX) $(CXXFLAGS) -Isrc $(CORE) tests/core.cpp -o $@

build/ori-tests-asan: $(CORE) src/ori.hpp src/adapter.hpp tests/core.cpp | build
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc $(CORE) tests/core.cpp -o $@

test: build/ori-replay build/ori-tests build/ori-probe
	./build/ori-tests
	$(PYTHON) tests/replay.py
	$(PYTHON) tests/evaluate.py --self-test

test-asan: build/ori-tests-asan
	./build/ori-tests-asan

bench: build/ori-probe
	./build/ori-probe --synthetic 18000

test-browser: build/ori-replay
	PYTHON="$(PYTHON)" node tests/browser.cjs

test-pages: pages
	PYTHON="$(PYTHON)" ORI_TEST_PAGES=1 node tests/browser.cjs

replay: build/ori-replay
	$(PYTHON) tests/replay.py --case "$(or $(CASE),retreat)"

demo: build/ori-replay
	$(PYTHON) demo/server.py

pages: build/pages/ori.mjs
	$(PYTHON) demo/build_pages.py build/pages

build/pages/ori.mjs: $(CORE) src/ori.hpp src/replay.cpp Makefile | build
	mkdir -p build/pages
	$(EMXX) -O2 -std=c++17 $(CORE) src/replay.cpp --no-entry -sENVIRONMENT=web \
		-sSTACK_SIZE=131072 -sEXPORTED_RUNTIME_METHODS=ccall,HEAPU8 -o $@

clean:
	rm -rf build
