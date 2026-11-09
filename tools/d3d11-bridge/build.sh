#!/bin/sh
# Builds tools/d3d11-bridge/d3d11.dll (x86-64 Windows) with MinGW-w64.
# Debian/Ubuntu: apt-get install gcc-mingw-w64-x86-64
set -eu
cd "$(dirname "$0")"
x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -shared \
    -o d3d11.dll bridge.c d3d11.def \
    -static-libgcc -Wl,--no-insert-timestamp -s
x86_64-w64-mingw32-objdump -p d3d11.dll | sed -n '/\[Ordinal\/Name Pointer\] Table/,/^$/p'
