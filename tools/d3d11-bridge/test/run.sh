#!/bin/sh
# Runs the bridge test under Wine (64-bit) in a throwaway prefix.
# Needs MinGW-w64 and wine64 (Debian/Ubuntu: apt-get install gcc-mingw-w64-x86-64 wine64).
#
#   sh run.sh         Madeira's layout: system32\d3d11.dll is built for another
#                     machine (as Madeira's ARM64 one is for an x64 game) and the
#                     usable d3d11.dll is in C:\windows\sysx64. A stand-in
#                     (fake_d3d11.c) plays DXMT there, so the device path is
#                     checked without a GPU.
#   sh run.sh wine    Plain Wine: the bridge finds Wine's own d3d11.dll in
#                     system32. Whether a device comes back depends on the
#                     machine's graphics.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
sh "$here/../build.sh" >/dev/null
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
x86_64-w64-mingw32-gcc -std=c11 -O1 -Wall -Wextra -o "$work/test_bridge.exe" "$here/test_bridge.c"
cp "$here/../d3d11.dll" "$work/"
WINE="${WINE:-$(command -v wine64 || command -v wine || echo /usr/lib/wine/wine64)}"
export WINEPREFIX="$work/prefix" WINEDEBUG="${WINEDEBUG:--all}" WINEDLLOVERRIDES="mscoree,mshtml=;d3d11=n,b"
"$WINE" wineboot -i >/dev/null 2>&1 || true
cd "$work"
if [ "${1:-}" = wine ]; then
    "$WINE" test_bridge.exe
    exit
fi
sys32="$WINEPREFIX/drive_c/windows/system32"
mkdir -p "$WINEPREFIX/drive_c/windows/sysx64"
x86_64-w64-mingw32-gcc -std=c11 -O1 -Wall -Wextra -shared -o "$WINEPREFIX/drive_c/windows/sysx64/d3d11.dll" \
    "$here/fake_d3d11.c" "$here/fake_d3d11.def"
# Mark system32's d3d11.dll as built for ARM64 (COFF machine 0xAA64).
pe="$(od -An -t u4 -j 60 -N 4 "$sys32/d3d11.dll" | tr -d ' ')"
printf '\144\252' | dd of="$sys32/d3d11.dll" bs=1 seek=$((pe + 4)) conv=notrunc 2>/dev/null
"$WINE" test_bridge.exe expect-device
