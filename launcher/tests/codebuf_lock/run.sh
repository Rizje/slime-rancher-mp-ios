#!/bin/sh
# SR2 Launcher: runs FEXCore's real code-buffer list functions, taken from a patched FEX checkout's
# Core.cpp, in a harness that models the waits seen on the phone (see harness.cpp). Each run must
# finish; a run that stops making progress for 2 s, or never finishes, fails the test.
#
#   sh launcher/tests/codebuf_lock/run.sh FEX_CHECKOUT [RUNS]
#
# Build 9's versions of these functions hung in 10 of 30 runs; build 11's finished 30 of 30.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
fex="$1"; runs="${2:-10}"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
python3 "$here/extract.py" "$fex/FEXCore/Source/Interface/Core/Core.cpp" "$work/impl.inc"
cp "$here/harness.cpp" "$work/"
${CXX:-c++} -std=c++20 -O1 -pthread -DIMPL='"impl.inc"' -I"$work" "$work/harness.cpp" -o "$work/h"
i=0
while [ "$i" -lt "$runs" ]; do
  i=$((i + 1))
  "$work/h" 2 > "$work/out" 2>&1 &
  pid=$!
  n=0
  while kill -0 "$pid" 2>/dev/null && [ "$n" -lt 150 ]; do sleep 0.1; n=$((n + 1)); done
  if kill -0 "$pid" 2>/dev/null; then
    kill -9 "$pid" 2>/dev/null || true
    echo "FAIL: run $i of $runs hung (killed after 15 s)" >&2; exit 1
  fi
  if ! wait "$pid"; then echo "FAIL: run $i of $runs: $(cat "$work/out")" >&2; exit 1; fi
done
echo "PASS: code buffer list: $runs of $runs runs finished ($(tail -1 "$work/out"))"
