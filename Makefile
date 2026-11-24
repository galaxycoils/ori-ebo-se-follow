CXX ?= c++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic
PYTHON ?= python3

.PHONY: all test test-browser replay demo clean
all: build/ori-replay

build:
	mkdir -p $@

build/ori-replay: src/ori.cpp src/dock.cpp src/ori.hpp src/replay.cpp | build
	$(CXX) $(CXXFLAGS) src/ori.cpp src/dock.cpp src/replay.cpp -o $@

build/ori-tests: src/ori.cpp src/dock.cpp src/ori.hpp tests/core.cpp | build
	$(CXX) $(CXXFLAGS) -Isrc src/ori.cpp src/dock.cpp tests/core.cpp -o $@

test: build/ori-replay build/ori-tests
	./build/ori-tests
	$(PYTHON) tests/replay.py

test-browser: build/ori-replay
	PYTHON="$(PYTHON)" node tests/browser.cjs

replay: build/ori-replay
	$(PYTHON) tests/replay.py --case "$(or $(CASE),retreat)"

demo: build/ori-replay
	$(PYTHON) demo/server.py

clean:
	rm -rf build
