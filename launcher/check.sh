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

echo "== 1b. no pipe2 on iOS (patch 0005)"
# Wine's config.h is written on the build Mac; a macOS 27 runner sets HAVE_PIPE2, and iOS 26 has no
# pipe2. No pipe2 call in Madeira's ntdll may be compiled for iOS (WINE_IOS).
python3 - "$work/patched" <<'PY'
import sys
from pathlib import Path
for name in ('server_ios.c', 'process_ios.c', 'thread_ios.c', 'signal_arm64_ios.c', 'virtual_ios.c', 'loader_ios.c'):
    stack = []   # per open #if: 'ios' (built for iOS), 'not-ios' (never built for iOS) or 'other'
    for n, line in enumerate((Path(sys.argv[1]) / 'build/ntdll-unix' / name).read_text().splitlines(), 1):
        d = line.strip()
        if d.startswith('#ifdef WINE_IOS'): stack.append('ios')
        elif d.startswith('#ifndef WINE_IOS') or (d.startswith('#if') and '!defined(WINE_IOS)' in d): stack.append('not-ios')
        elif d.startswith('#if'): stack.append('other')
        elif d.startswith('#else') and stack: stack[-1] = {'ios': 'not-ios', 'not-ios': 'ios'}.get(stack[-1], 'other')
        elif d.startswith('#endif') and stack: stack.pop()
        elif 'pipe2(' in line and not line.lstrip().startswith(('*', '/*', '//')):
            assert 'not-ios' in stack, '%s:%d calls pipe2 in code built for iOS' % (name, n)
print('PASS: no pipe2 call is built for iOS')
PY
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
assert len(settings) == len(lines) == 6, lines
for key in settings:
    if key.startswith('env.FEX_'):   # FEX's own option: Madeira passes it through; its lists use it
        uses = len(re.findall(r'^\s*%s = ' % re.escape(key), lists, re.M))
        assert uses >= 2, (key, 'no other Madeira list uses it')
    else:
        assert 'key: "%s"' % key in catalog, (key, 'not in ConfigCatalog.generated.swift')
# Not diagnostics only: MADEIRA_PROBES below 2 never starts the thread sampler, and MADEIRA_LEAN_DIAG skips its
# ios_stuck_store_check, which completes a store stuck across a 16 KB page (ml1288). The tested runs had it.
assert not any(k in settings for k in ('env.MADEIRA_PROBES', 'env.MADEIRA_LEAN_DIAG')), settings
assert 'if (probes >= 2) dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ ios_thread_sampler_main(); });' in server
assert 'if (stuck_check && !ios_lean_diag) ios_stuck_store_check();' in server
tested = {'env.MADEIRA_EC_HOOK_TRACE': '0', 'donor-pool-guard': '1', 'env.FEX_VECTORTSOENABLED': '0',
          'env.MADEIRA_FPS_GOV_MW': '2000', 'env.MADEIRA_FPS_GOV_MAX': '30', 'env.MADEIRA_FPS_GOV_MIN': '20'}
assert settings == tested, settings   # the phone's [game-cfg] of 2026-11-10, less FEX's own default memcpyset=0
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
print('PASS: the %d list lines are the tested config, and they and the 3 memory defaults are keys Madeira reads' % len(lines))
PY

echo "== 3. Madeira's host checks, stock against patched"
python3 "$here/tests/compare_host_checks.py" ${all[@]+"${all[@]}"} "$work/stock" "$work/patched"

echo "== 5. the FEX patches apply, in order, on Madeira's FEX pin (patch 0006)"
# build/fex-patches/apply.sh puts Madeira's 0001 and the launcher's 0002 on the FEX submodule before
# xtajit64.dll is built. The submodule is not checked out here: fetch that one commit and try them.
fex_pin="$(git -C "$work/patched" rev-parse "HEAD:FEX")"
fex_url="$(git -C "$work/patched" config -f .gitmodules submodule.FEX.url)"
git init -q "$work/fex" && git -C "$work/fex" fetch -q --depth 1 "$fex_url" "$fex_pin" &&
  git -C "$work/fex" checkout -q FETCH_HEAD || { echo "check.sh: cannot fetch $fex_url $fex_pin" >&2; exit 1; }
fex_patches=("$work"/patched/build/fex-patches/*.patch)
[[ "${#fex_patches[@]}" == 2 && "$(basename "${fex_patches[1]}")" == 0002-fexcore-code-buffer-list-walk-re-entry.patch ]] ||
  { echo "check.sh: expected Madeira's 0001 and the launcher's 0002 in build/fex-patches, got: ${fex_patches[*]}" >&2; exit 1; }
for p in "${fex_patches[@]}"; do
  git -C "$work/fex" apply --check "$p" || { echo "check.sh: $(basename "$p") does not apply on FEX $fex_pin" >&2; exit 1; }
  git -C "$work/fex" apply "$p"
done
grep -q 'CodeBufferListOwner.load(std::memory_order_relaxed) == Self' "$work/fex/FEXCore/Source/Interface/Core/Core.cpp" &&
  grep -q '\[inval-nested-skip\]' "$work/fex/Source/Windows/Common/InvalidationTracker.cpp" &&
  grep -q 'std::atomic<int32_t> CodeBufferListOwner {0};' "$work/fex/FEXCore/Source/Interface/Context/Context.h" &&
  grep -q 'inline int32_t gettid() {' "$work/fex/FEXHeaderUtils/FEXHeaderUtils/Syscalls.h" &&
  grep -q '#include <FEXHeaderUtils/Syscalls.h>' "$work/fex/FEXCore/Source/Interface/Core/Core.cpp" ||
  { echo "check.sh: the patched FEX source lacks what patch 0006 relies on" >&2; exit 1; }
# A patch that is already in the tree is skipped, not applied twice: apply.sh's test for that.
git -C "$work/fex" apply --reverse --check "${fex_patches[1]}" || { echo "check.sh: 0002 cannot be recognised as applied" >&2; exit 1; }
echo "PASS: ${#fex_patches[@]} FEX patches apply on willfaust/FEX ${fex_pin:0:7}"

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
