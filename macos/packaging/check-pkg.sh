#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Checks a built Ozzy-CMS600 package without installing it.
set -euo pipefail
PKG="$1"
VERSION="$2"
BUNDLE_VERSION="${VERSION%%[-+]*}"
fail() { echo "FAIL: $*"; exit 1; }

[ -f "$PKG" ] || fail "no package at $PKG"
WORK="$(mktemp -d "$(dirname "$PKG")/check.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
pkgutil --expand-full "$PKG" "$WORK/x"
COMP="$WORK/x/ozzy-userspace.pkg"
[ -d "$COMP" ] || fail "component ozzy-userspace.pkg missing"

grep -q 'hostArchitectures="arm64"' "$WORK/x/Distribution" || fail "Distribution has no arm64 host check"
grep -q '<os-version min="26.1"' "$WORK/x/Distribution" || fail "Distribution has no macOS 26.1 check"
grep -q 'identifier="com.ozzy.userspace"' "$COMP/PackageInfo" || fail "wrong package identifier"
grep -q "version=\"$VERSION\"" "$COMP/PackageInfo" || fail "package version is not $VERSION"
relocate="$(sed -n '/<relocate>/,/<\/relocate>/p' "$COMP/PackageInfo")"
if grep -q 'bundle id' <<<"$relocate"; then
	fail "a bundle is relocatable"
fi

P="$COMP/Payload"
expected="./Library/Application Support/Ozzy/uninstall.sh
./Library/Audio/MIDI Drivers/OzzyMIDI.plugin
./Library/Audio/Plug-Ins/HAL/OzzyHAL.driver
./Library/LaunchDaemons/com.ozzy.daemon.plist
./usr/local/bin/OzzyDaemon"
actual="$(cd "$P" && find . -type f | sed -E 's#(\.driver|\.plugin)/.*#\1#' | LC_ALL=C sort -u)"
[ "$actual" = "$expected" ] || fail "payload differs:
$actual"

for b in "$P/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver" "$P/Library/Audio/MIDI Drivers/OzzyMIDI.plugin"; do
	v="$(plutil -extract CFBundleShortVersionString raw "$b/Contents/Info.plist")"
	[ "$v" = "$BUNDLE_VERSION" ] || fail "$(basename "$b") version $v, expected $BUNDLE_VERSION"
done
D="$P/usr/local/bin/OzzyDaemon"
# The daemon logs the version it was built as (OZZY_GIT_VERSION)
strings "$D" | grep -qx -- "$VERSION" || fail "OzzyDaemon is not built as $VERSION"

for b in "$P/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver" "$P/Library/Audio/MIDI Drivers/OzzyMIDI.plugin" "$D"; do
	codesign --verify --strict "$b" || fail "$(basename "$b") does not verify"
	sig="$(codesign -dv "$b" 2>&1)"
	grep -qx 'Signature=adhoc' <<<"$sig" || fail "$(basename "$b") is not ad-hoc signed"
	if [ -d "$b" ]; then
		exe="$b/Contents/MacOS/$(basename "${b%.*}")"
	else
		exe="$b"
	fi
	archs="$(lipo -archs "$exe" | tr " " "\n" | LC_ALL=C sort | xargs)"
	[ "$archs" = "arm64" ] || fail "$(basename "$b") has architectures $archs, expected arm64"
done
sig="$(codesign -dv "$D" 2>&1)"
grep -qx 'Identifier=com.ozzy.daemon' <<<"$sig" || fail "OzzyDaemon is not signed as com.ozzy.daemon"

L="$P/Library/LaunchDaemons/com.ozzy.daemon.plist"
plutil -lint "$L" >/dev/null || fail "LaunchDaemon plist invalid"
[ "$(plutil -extract ProgramArguments.0 raw "$L")" = /usr/local/bin/OzzyDaemon ] \
	|| fail "LaunchDaemon does not run /usr/local/bin/OzzyDaemon"
# The HAL calls the daemon's mach service, which its sandbox allows only when listed
[ "$(plutil -extract AudioServerPlugIn_MachServices.0 raw "$P/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver/Contents/Info.plist")" = com.ozzy.daemon ] \
	|| fail "OzzyHAL does not list the com.ozzy.daemon mach service"
# launchd runs a LaunchDaemon, and root its binary, only if root alone can change them
bom="$(lsbom -p MUGf "$COMP/Bom")"
for e in $'drwxr-xr-x \troot\twheel\t./Library/Application Support/Ozzy' \
	$'-rwxr-xr-x \troot\twheel\t./usr/local/bin/OzzyDaemon' \
	$'-rw-r--r-- \troot\twheel\t./Library/LaunchDaemons/com.ozzy.daemon.plist'; do
	grep -qxF -- "$e" <<<"$bom" || fail "not in the package as ${e%%$'\t'*} root:wheel: ${e##*$'\t'}"
done
for s in preinstall postinstall uninstall.sh; do
	[ -x "$COMP/Scripts/$s" ] || fail "script $s missing or not executable"
done
[ -x "$P/Library/Application Support/Ozzy/uninstall.sh" ] || fail "installed uninstall.sh not executable"
echo "package checks passed: $PKG"
