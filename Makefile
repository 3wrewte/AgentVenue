CXX      := g++
CXXFLAGS := -std=c++20 -O2 -g -Wall -Wextra -I. -IOoOobookCPP/src -Iagents -Irunner -Ilibs/doctest

SIM      := build/agentvenue
SIM_SRC  := runner/sim_main.cpp
HEADERS  := $(wildcard agents/*.hpp agents/*/*.hpp runner/*.hpp)

TEST_MAIN := test/test_main.cpp
TEST_SRCS := $(filter-out $(TEST_MAIN), $(wildcard test/*.cpp))
TEST_BIN  := build/tests

.PHONY: all sim test run clean

all: sim

sim: $(SIM)
$(SIM): $(SIM_SRC) $(HEADERS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SIM_SRC) -o $@

$(TEST_BIN): $(TEST_MAIN) $(TEST_SRCS) $(HEADERS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(TEST_MAIN) $(TEST_SRCS) -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)

run: $(SIM)
	@mkdir -p build/sim
	./$(SIM)

clean:
	rm -rf build
