#!/bin/sh
# SR2 Launcher: builds usd_probe_test.c natively and runs it against patch 0001's helper.
#
#   sh launcher/tests/run.sh               the helper as patch 0001 adds it
#   sh launcher/tests/run.sh SIGNAL_C      the helper in a patched tree's
#                                          build/ntdll-unix/signal_arm64_ios.c
# Needs a C compiler (cc, or CC=...).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
if [ $# -gt 0 ]; then
    sed -n '/^\/\* sr2:usd-probe:begin \*\//,/^\/\* sr2:usd-probe:end \*\//p' "$1" > "$work/helper.c"
else
    sed -n '/^+\/\* sr2:usd-probe:begin \*\//,/^+\/\* sr2:usd-probe:end \*\//{s/^+//;p;}' \
        "$here"/../patches/0001-*.patch > "$work/helper.c"
fi
[ "$(grep -c 'sr2:usd-probe:' "$work/helper.c")" = 2 ] || { echo "run.sh: the sr2:usd-probe block was not found" >&2; exit 1; }
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O2 -I"$work" -o "$work/usd_probe_test" "$here/usd_probe_test.c"
"$work/usd_probe_test"
