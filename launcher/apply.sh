#!/usr/bin/env bash
# SR2 Launcher: puts the patches in launcher/patches on a checkout of Madeira at the commit
# launcher/UPSTREAM names, as one local commit. The commit matters: Madeira's IPA cache is keyed
# by the tree (git rev-parse 'HEAD^{tree}'), so a patched tree never reuses a stock IPA.
#
#   bash launcher/apply.sh [MADEIRA_DIR]      (default: the current directory)
#
# SR2_STOCK=true checks the checkout and applies nothing: plain Madeira, for comparison.
# The patches apply exactly or not at all: git apply takes no fuzz, and with --check first
# nothing is changed when one of them does not fit.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tree="$(cd "${1:-.}" && pwd)"
read -r repo pin tag < "$here/UPSTREAM"
fail() { echo "apply.sh: $*" >&2; exit 1; }

head="$(git -C "$tree" rev-parse HEAD)" || fail "$tree is not a git checkout"
[[ "$head" == "$pin" ]] || fail "$tree is at $head, not $repo $pin ($tag)"
[[ -z "$(git -C "$tree" status --porcelain --untracked-files=no --ignore-submodules=all)" ]] ||
  fail "$tree has local changes; the patches go on a clean checkout"

if [[ "${SR2_STOCK:-false}" == true ]]; then
  echo "apply.sh: SR2_STOCK=true: $repo $tag stays unpatched (tree $(git -C "$tree" rev-parse 'HEAD^{tree}'))"
  exit 0
fi

patches=("$here"/patches/*.patch)
[[ -f "${patches[0]}" ]] || fail "no patches in $here/patches"
# The series as one input to one git apply: a patch sees the changes of the ones before it in
# the same file (separate patch files would each be checked against the original), and nothing
# is written unless every hunk applies.
cat "${patches[@]}" | git -C "$tree" apply --index --check || fail "the patches do not apply to $repo $tag"
cat "${patches[@]}" | git -C "$tree" apply --index
names=()
for p in "${patches[@]}"; do names+=("$(basename "$p" .patch)"); done
git -C "$tree" -c user.name="SR2 Launcher" -c user.email="sr2-launcher@users.noreply.github.com" \
  commit -q --no-verify -m "SR2 Launcher: Madeira $tag with ${#patches[@]} patches" -m "$(printf '%s\n' "${names[@]}")"
echo "apply.sh: ${#patches[@]} patches on $repo $tag; tree $(git -C "$tree" rev-parse 'HEAD^{tree}')"
