# stand_runner: the campaign watchtower and the form that writes a run. No dependencies beyond
# the C++20 standard library and POSIX, deliberately - it has to build on the load generator
# without installing anything.
CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++20 -Wall -Wextra -Wpedantic

sources = stand_runner.cpp run_file.cpp run_form.cpp run_control.cpp tui.cpp

# Installed as a symlink, not a copy: the program finds stand_run.sh by walking up from its own
# real path, and /proc/self/exe resolves the link back to the checkout.
PREFIX ?= $(HOME)/.local

all: stand_runner

stand_runner: $(sources) frame.h run_file.h run_form.h run_control.h tui.h theme.h
	$(CXX) $(CXXFLAGS) -o $@ $(sources)

install: stand_runner
	install -d $(PREFIX)/bin
	ln -sf $(abspath stand_runner) $(PREFIX)/bin/stand_runner
	@echo "installed: $(PREFIX)/bin/stand_runner -> $(abspath stand_runner)"

clean:
	rm -f stand_runner

.PHONY: all clean install
