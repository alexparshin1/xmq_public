# stand_runner: the campaign watchtower and the form that writes a run. No dependencies beyond
# the C++20 standard library and POSIX, deliberately - it has to build on the load generator
# without installing anything.
CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++20 -Wall -Wextra -Wpedantic

sources = stand_runner.cpp run_file.cpp run_form.cpp tui.cpp

all: stand_runner

stand_runner: $(sources) run_file.h run_form.h tui.h
	$(CXX) $(CXXFLAGS) -o $@ $(sources)

clean:
	rm -f stand_runner

.PHONY: all clean
