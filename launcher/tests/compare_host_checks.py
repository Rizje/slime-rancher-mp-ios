#!/usr/bin/env python3
"""SR2 Launcher: Madeira's own host checks on stock Madeira and on the patched tree, compared.

    python3 launcher/tests/compare_host_checks.py STOCK_TREE PATCHED_TREE [--all] [--timeout SECONDS]

STOCK_TREE is a checkout of the commit in launcher/UPSTREAM; PATCHED_TREE is the same commit with
launcher/apply.sh run on it. Each check (tests/host/check-*.py) runs in both, one at a time, and the
script fails when a check that passes on stock Madeira fails with the patches. A check that fails on
both trees needs something this machine lacks (swiftc, xcrun, plutil, a Wine or FEX tree), says
nothing about the patches, and is listed apart.

By default only the checks that read a file the patches change, or that a patch changes itself, run;
--all runs every check. Logs go to a temporary folder named at the end.
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('stock', type=Path)
parser.add_argument('patched', type=Path)
parser.add_argument('--all', action='store_true', help='every check, not only those that read a changed file')
parser.add_argument('--timeout', type=int, default=600, help='seconds per check and tree (default 600)')
args = parser.parse_args()
stock, patched = args.stock.resolve(), args.patched.resolve()


def git(tree, *argv):
    return subprocess.run(['git', '-C', str(tree), *argv], check=True, capture_output=True, text=True).stdout.strip()


base = git(stock, 'rev-parse', 'HEAD')
if git(patched, 'rev-parse', 'HEAD~1') != base:
    sys.exit('compare: %s is not one commit (launcher/apply.sh) on %s' % (patched, base))
changed = [line for line in git(patched, 'diff', '--name-only', base, 'HEAD').splitlines() if line]
names = sorted({Path(path).name for path in changed})
checks = sorted(p.name for p in (patched / 'tests/host').glob('check-*.py'))
if not args.all:
    checks = [name for name in checks
              if 'tests/host/' + name in changed
              or any(n in (patched / 'tests/host' / name).read_text(errors='replace') for n in names)]
print('compare: %d changed files (%s); %d checks%s' % (len(changed), ', '.join(names), len(checks),
                                                        '' if args.all else ' read one of them'))

logs = Path(tempfile.mkdtemp(prefix='sr2-host-checks-'))


def run(tree, name):
    """0 passed, 1 failed, 2 timed out; and the log's last line."""
    log = logs / ('%s.%s.log' % (name, 'stock' if tree == stock else 'patched'))
    with open(log, 'w') as out:
        try:
            code = subprocess.run([sys.executable, str(tree / 'tests/host' / name)], cwd=tree, stdout=out,
                                  stderr=subprocess.STDOUT, timeout=args.timeout,
                                  env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1')).returncode
            state = 0 if code == 0 else 1
        except subprocess.TimeoutExpired:
            state = 2
    lines = [line for line in log.read_text(errors='replace').splitlines() if line.strip()]
    return state, (lines[-1][:160] if lines else '')


regressions, fixed, both_fail, both_pass = [], [], [], []
start = time.time()
for name in checks:
    s, s_last = run(stock, name)
    p, p_last = run(patched, name)
    if s == 0 and p != 0:
        regressions.append((name, p_last))
    elif s != 0 and p == 0:
        fixed.append(name)
    elif s != 0:
        both_fail.append((name, s_last))
    else:
        both_pass.append(name)
    print('  %-48s stock %-8s patched %s' % (name, ('pass', 'FAIL', 'TIMEOUT')[s], ('pass', 'FAIL', 'TIMEOUT')[p]),
          flush=True)

print('compare: %d pass on both, %d fail on both (missing tools), %d pass only with the patches, '
      '%d REGRESSIONS (%.0f s; logs in %s)' % (len(both_pass), len(both_fail), len(fixed), len(regressions),
                                               time.time() - start, logs))
for name, last in both_fail:
    print('  fails on stock too: %s: %s' % (name, last))
for name in fixed:
    print('  passes only with the patches: %s' % name)
for name, last in regressions:
    print('  REGRESSION: %s: %s' % (name, last))
if regressions:
    sys.exit(1)
if not both_pass and checks:
    sys.exit('compare: no check passed on either tree, so nothing was compared')
