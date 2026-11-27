# xmq_runner: the campaign watchtower and the form that writes a run. No dependencies beyond
# the C++20 standard library and POSIX, deliberately - it has to build on the load generator
# without installing anything.
CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++20 -Wall -Wextra -Wpedantic

sources = stand_runner.cpp run_file.cpp run_form.cpp run_control.cpp tui.cpp

# Installed as a symlink, not a copy: the program finds stand_run.sh by walking up from its own
# real path, and /proc/self/exe resolves the link back to the checkout.
PREFIX ?= $(HOME)/.local

all: xmq_runner

xmq_runner: $(sources) frame.h run_file.h run_form.h run_control.h tui.h theme.h
	$(CXX) $(CXXFLAGS) -o $@ $(sources)

install: xmq_runner
	install -d $(PREFIX)/bin
	ln -sf $(abspath xmq_runner) $(PREFIX)/bin/xmq_runner
	@echo "installed: $(PREFIX)/bin/xmq_runner -> $(abspath xmq_runner)"

clean:
	rm -f xmq_runner

.PHONY: all clean install
