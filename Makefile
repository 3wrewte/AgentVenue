CXX      := g++
CXXFLAGS := -std=c++2c -O2 -g -Wall -Wextra -I. -IOoOobookCPP/src -Iagents -Irunner -Ilibs/doctest

SIM      := build/agentvenue
SIM_SRC  := runner/sim_main.cpp runner/venue.cpp
GRID     := build/mm_arb_grid
GRID_SRC := runner/mm_arb_grid.cpp runner/venue.cpp
CTA_GRID     := build/mm_arb_cta_grid
CTA_GRID_SRC := runner/mm_arb_cta_grid.cpp runner/venue.cpp
HEADERS  := $(wildcard agents/*.hpp agents/*/*.hpp runner/*.hpp)

TEST_MAIN := test/test_main.cpp
TEST_SRCS := $(filter-out $(TEST_MAIN), $(wildcard test/*.cpp)) runner/venue.cpp
TEST_BIN  := build/tests

.PHONY: all sim grid cta-grid test run run-grid run-cta-grid clean

all: sim grid cta-grid

sim: $(SIM)
$(SIM): $(SIM_SRC) $(HEADERS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SIM_SRC) -o $@

grid: $(GRID)
$(GRID): $(GRID_SRC) $(HEADERS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(GRID_SRC) -o $@

cta-grid: $(CTA_GRID)
$(CTA_GRID): $(CTA_GRID_SRC) $(HEADERS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(CTA_GRID_SRC) -o $@

$(TEST_BIN): $(TEST_MAIN) $(TEST_SRCS) $(HEADERS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(TEST_MAIN) $(TEST_SRCS) -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)

run: $(SIM)
	@mkdir -p build/sim
	./$(SIM)

run-grid: $(GRID)
	./$(GRID)

run-cta-grid: $(CTA_GRID)
	./$(CTA_GRID)

clean:
	rm -rf build
