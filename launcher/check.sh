#!/usr/bin/env bash
# SR2 Launcher: every check that runs without building the app.
#
#   bash launcher/check.sh [--all] [MADEIRA_CLONE]
#
# MADEIRA_CLONE is a git clone of willfaust/Madeira that has the commit in launcher/UPSTREAM; without
# one, Madeira is cloned into a temporary folder (no submodules). Two worktrees are made at that
# commit, apply.sh patches one of them, and then:
#   1. tests/run.sh: patch 0001's helper at its boundaries, from the patch and from the patched file
#   2. every line of the Slime Rancher 2 list and every SR2Launcher default is a key Madeira reads,
#      with a value it accepts
#   3. Madeira's host checks that read a file the patches change (--all: every check), on stock and
#      patched; a check that passes on stock must pass patched (tests/compare_host_checks.py)
#   4. with SR2_REQUIRE_SWIFT=1 (CI on macOS): Madeira's Swift fixture for the recommended settings,
#      which compiles GameRecommendations*.swift and runs the Slime Rancher 2 cases, must run and pass
# Needs git, bash, python3 and a C compiler. With swiftc on PATH (macOS) the Swift fixtures run too.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
all=()
if [[ "${1:-}" == --all ]]; then all=(--all); shift; fi
read -r repo pin tag < "$here/UPSTREAM"
work="$(mktemp -d)"
clone="${1:-}"
cleanup() {
  if [[ -n "$clone" ]]; then
    git -C "$clone" worktree remove --force "$work/stock" 2>/dev/null || true
    git -C "$clone" worktree remove --force "$work/patched" 2>/dev/null || true
  fi
  rm -rf "$work"
}
trap cleanup EXIT

if [[ -z "$clone" ]]; then
  clone="$work/madeira"
  git clone -q --no-checkout "https://github.com/$repo.git" "$clone"
fi
clone="$(cd "$clone" && pwd)"
git -C "$clone" cat-file -e "$pin^{commit}" 2>/dev/null || git -C "$clone" fetch -q origin "$pin"
git -C "$clone" worktree add -q --detach "$work/stock" "$pin"
git -C "$clone" worktree add -q --detach "$work/patched" "$pin"
bash "$here/apply.sh" "$work/patched"

echo "== 1. patch 0001's helper"
sh "$here/tests/run.sh" | tail -1
sh "$here/tests/run.sh" "$work/patched/build/ntdll-unix/signal_arm64_ios.c" | tail -1

echo "== 2. the settings are keys and values Madeira reads"
python3 - "$work/patched" <<'PY'
import re, sys
from pathlib import Path
tree = Path(sys.argv[1])
app = tree / 'app/Madeira'
lists = (app / 'GameRecommendations.swift').read_text()
catalog = (app / 'ConfigCatalog.generated.swift').read_text()
library = (app / 'Library.swift').read_text()
server = (tree / 'build/ntdll-unix/server_ios.c').read_text()
config = (app / 'MadeiraConfig.swift').read_text()

block = lists.split('static let slimeRancher2 = GameRecommendation(', 1)[1].split('""",', 1)[0]
lines = [l.strip() for l in block.split('"""', 1)[1].splitlines() if l.strip() and not l.strip().startswith('#')]
settings = dict(tuple(part.strip() for part in l.split('=', 1)) for l in lines)
assert len(settings) == len(lines) == 8, lines
for key in settings:
    if key.startswith('env.FEX_'):   # FEX's own option: Madeira passes it through; its lists use it
        uses = len(re.findall(r'^\s*%s = ' % re.escape(key), lists, re.M))
        assert uses >= 2, (key, 'no other Madeira list uses it')
    else:
        assert 'key: "%s"' % key in catalog, (key, 'not in ConfigCatalog.generated.swift')
assert settings['env.MADEIRA_PROBES'] == 'light' and '!strcmp(probes_env, "light")' in server
assert settings['env.MADEIRA_LEAN_DIAG'] == '1' and 'getenv("MADEIRA_LEAN_DIAG")' in server
for name in ('MW', 'MAX', 'MIN'):
    assert 'getenv( "MADEIRA_FPS_GOV_%s" )' % name in server, name
assert int(settings['env.MADEIRA_FPS_GOV_MIN']) <= int(settings['env.MADEIRA_FPS_GOV_MAX']) == 30
tail = lists.split('static let slimeRancher2 = GameRecommendation(', 1)[1].split(')\n', 1)[0]
assert 'fpsMode: 3, resolution: "960x540", display: "fit", arguments: "", metalFXUpscale: 2' in tail, tail[-160:]
assert 'FPS limit: 1 = 60, 3 = 30' in library
assert 'SteamTitle(appIDs: [slimeRancher2AppID], folders: ["slime rancher 2"],' in lists
assert 'programs: ["slimerancher2.exe"], steamPrograms: [], list: slimeRancher2)' in lists

defaults = dict(re.findall(r'\("([^"]+)", "([^"]+)"\)', config.split('enum SR2Launcher', 1)[1].split('static func', 1)[0]))
assert defaults == {'swap-mb': '4096', 'env.MADEIRA_SWAP_COVERAGE': 'wide', 'donor-pool-mb': '2048'}, defaults
for key in defaults:
    assert 'key: "%s"' % key in catalog, key
assert 'static let swapChoices = [0, 1024, 2048, 3072, 4096]' in library
assert 'static let donorPoolChoices = [0, 2048, 4096, 6144]' in library
assert '("wide", L("1 MB+ and overflow"))' in library
print('PASS: %d list lines and 3 memory defaults are keys Madeira reads, with values it offers' % len(lines))
PY

echo "== 3. Madeira's host checks, stock against patched"
python3 "$here/tests/compare_host_checks.py" ${all[@]+"${all[@]}"} "$work/stock" "$work/patched"

if [[ "${SR2_REQUIRE_SWIFT:-0}" == 1 ]]; then
  echo "== 4. the recommended-settings Swift fixture, patched (--require-swift)"
  if ! (cd "$work/patched" && python3 tests/host/check-game-recommendations.py --require-swift); then
    echo "check.sh: the fixture fails with the patches; on stock Madeira it:" >&2
    (cd "$work/stock" && python3 tests/host/check-game-recommendations.py --require-swift) >&2 &&
      echo "passes" >&2 || echo "fails too" >&2
    exit 1
  fi
fi
echo "check.sh: all checks passed ($repo $tag + launcher/patches)"
