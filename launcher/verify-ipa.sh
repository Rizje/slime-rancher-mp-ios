#!/usr/bin/env bash
# SR2 Launcher: checks a built IPA before it is published (macOS: unzip, plutil, codesign).
#
#   bash launcher/verify-ipa.sh IPA launcher|stock BUNDLE_ID ENTITLEMENTS
#
# ENTITLEMENTS is the tree's app/Madeira/Madeira.entitlements, which package-ipa.sh signs with.
# Checks: the app and its extensions carry BUNDLE_ID (the extensions as BUNDLE_ID.<name>), version
# 0.2.0, the signature's entitlements are exactly that file's, and the name, build stamp and
# patched code are the launcher's (or, for stock, Madeira's own and none of the launcher's). The
# launcher must have its code in the Madeira executable, without Xcode's debug dylib.
set -euo pipefail
ipa="$1" variant="$2" bundle="$3" entitlements="$4"
fail() { echo "verify-ipa: $*" >&2; exit 1; }
[[ "$variant" == launcher || "$variant" == stock ]] || fail "variant must be launcher or stock"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
unzip -q "$ipa" -d "$work"
app="$work/Payload/Madeira.app"
[[ -d "$app" && -f "$app/Madeira" ]] || fail "no Payload/Madeira.app with its executable"
key() { plutil -extract "$2" raw -o - "$1/Info.plist" 2>/dev/null || true; }

[[ "$(key "$app" CFBundleIdentifier)" == "$bundle" ]] || fail "bundle id $(key "$app" CFBundleIdentifier), expected $bundle"
extensions=0
for ext in "$app"/PlugIns/*.appex; do
  [[ -d "$ext" ]] || continue
  id="$(key "$ext" CFBundleIdentifier)"
  [[ "$id" == "$bundle".* ]] || fail "$(basename "$ext") has bundle id $id, not under $bundle"
  extensions=$((extensions + 1))
done
[[ "$extensions" -ge 1 ]] || fail "no app extensions (the JIT helper) in PlugIns"
[[ "$(key "$app" CFBundleShortVersionString)" == 0.2.0 ]] || fail "version $(key "$app" CFBundleShortVersionString)"

codesign -d --entitlements - --xml "$app" > "$work/signed.plist" 2>/dev/null || fail "the app has no signature"
python3 - "$work/signed.plist" "$entitlements" <<'PY' || fail "the signature's entitlements differ from Madeira.entitlements"
import plistlib, sys
signed, expected = (plistlib.load(open(path, 'rb')) for path in sys.argv[1:])
if signed != expected:
    print('signed:', signed, '\nexpected:', expected, file=sys.stderr)
    sys.exit(1)
assert signed.get('com.apple.security.cs.allow-jit') is True
PY

# Every system function the app links weakly must exist on iOS 26: one the phone lacks resolves to
# 0 and crashes on its first call. Build 5 had pipe2 (Wine's configure ran on a macOS 27 runner) and
# closed the app as Wine made its first pipe. These five are in iOS 17.4 and later.
allowed_weak='^_(os_sync_wait_on_address|os_sync_wait_on_address_with_timeout|os_sync_wake_by_address_all|os_sync_wake_by_address_any|_availability_version_check)$'
weak_system() { nm -m -u "$1" 2>/dev/null | awk '/weak external/ && /\(from libSystem\)/ {print $4}'; }
unknown_weak="$(weak_system "$app/Madeira" | grep -v -E "$allowed_weak" | tr '\n' ' ' || true)"
unknown_weak="${unknown_weak% }"

# Strings the patches put in the code; grep -r finds them in whichever binary links them.
markers=("usd-probe-skips=" "[exc-census] sr2" "MADEIRA_USD_PROBE" "(SR2 Launcher's list)" "sr2launcher.applied"
         "sr2launcher.seeded.1" "SR2 Launcher is a modified version of Madeira 0.2.0")
found() { grep -r -a -q -F -- "$1" "$app"; }
name="$(key "$app" CFBundleDisplayName)" stamp="$(key "$app" MadeiraBuild)"
if [[ "$variant" == launcher ]]; then
  # One executable, as in Madeira's 0.2.0 release: the code is not in a debug dylib behind a stub.
  [[ ! -e "$app/Madeira.debug.dylib" && ! -e "$app/__preview.dylib" ]] || fail "the app has Xcode's debug dylib layout"
  grep -a -q -F -- "[exc-census] sr2" "$app/Madeira" || fail "the patched code is not in the Madeira executable"
  [[ -z "$unknown_weak" ]] || fail "weakly linked system functions iOS 26 may lack: $unknown_weak"
  [[ "$name" == "SR2 Launcher" ]] || fail "display name '$name', expected SR2 Launcher"
  [[ "$stamp" == "0.2.0 · SR2 Launcher 1 · "* ]] || fail "build stamp '$stamp'"
  for m in "${markers[@]}"; do found "$m" || fail "'$m' is not in the app: a patch is missing from the build"; done
  # Patch 0006 is in FEX's ARM64EC module, built by the windows runtime component from the FEX pin
  # plus build/fex-patches. Its log tag is in that DLL and nowhere else.
  for m in "[codebuf-nested]" "[inval-nested-skip]"; do
    grep -a -q -F -- "$m" "$app/arm64ec-windows/xtajit64.dll" ||
      fail "'$m' is not in arm64ec-windows/xtajit64.dll: it was not rebuilt with build/fex-patches/0002"
  done
else
  ! grep -a -q -F -- "[inval-nested-skip]" "$app/arm64ec-windows/xtajit64.dll" || fail "the stock xtajit64.dll has patch 0006's code"
  [[ "$name" == Madeira ]] || fail "display name '$name', expected Madeira"
  [[ -z "$stamp" ]] || fail "stock build has a stamp '$stamp'"
  for m in "${markers[@]}"; do ! found "$m" || fail "'$m' is in the stock app"; done
fi
summary="$bundle with $extensions extension(s), named '$name'"
[[ -z "$stamp" ]] || summary+=", stamp '$stamp'"
if [[ "$variant" == launcher ]]; then summary+=", all ${#markers[@]} patch markers, no unknown weak system imports"
else summary+=", no patch markers${unknown_weak:+, weak system imports: $unknown_weak}"; fi
echo "verify-ipa: $variant $(basename "$ipa"): $summary, entitlements as Madeira.entitlements"
