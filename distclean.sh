#!/bin/sh

# GNU make is required to run the CMake-generated Makefiles (they use GNU-only syntax).
# Linux's default `make` already is GNU make; BSD's default `make` is bmake, so prefer
# `gmake` there when it's installed.
if command -v gmake >/dev/null 2>&1; then
    MAKE=gmake
else
    MAKE=make
fi

[ -f Makefile ] && $MAKE clean

[ -f CMakeCache.txt ] && rm -f CMakeCache.txt

# POSIX/BSD find requires an explicit starting path - GNU find is alone in defaulting it to
# '.', which is why the previous `find -name ...` cleaned nothing here and reported a usage
# error for every name it was given.
#
# Directories are pruned so find does not descend into what it is about to remove, and
# `-exec ... +` batches the removals instead of forking one rm per match.
find . \( \
    -name CMakeFiles -o \
    -name .cmake -o \
    -name .scannerwork -o \
    -name bw-output -o \
    -name cmake-build-debug -o \
    -name Debug -o \
    -name Release \
  \) -type d -prune -exec rm -rf {} +

find . \( \
    -name Makefile -o \
    -name install_manifest.txt -o \
    -name cmake_install.cmake -o \
    -name compile_commands.json \
  \) -type f -exec rm -f {} +

[ -d lib ] && rm -rf lib/*

rm -f callgrind*
