# stand_runner: the campaign watchtower. No dependencies beyond the C++20 standard library and
# POSIX, deliberately - it has to build on the load generator without installing anything.
CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++20 -Wall -Wextra -Wpedantic

all: stand_runner

stand_runner: stand_runner.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f stand_runner

.PHONY: all clean
