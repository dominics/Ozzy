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
expected="./Library/Application Support/Ozzy/OzzyDaemon
./Library/Application Support/Ozzy/uninstall.sh
./Library/Audio/MIDI Drivers/OzzyMIDI.plugin
./Library/Audio/Plug-Ins/HAL/OzzyHAL.driver
./Library/LaunchAgents/com.ozzy.daemon.plist"
actual="$(cd "$P" && find . -type f | sed -E 's#(\.driver|\.plugin)/.*#\1#' | LC_ALL=C sort -u)"
[ "$actual" = "$expected" ] || fail "payload differs:
$actual"

for b in "$P/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver" "$P/Library/Audio/MIDI Drivers/OzzyMIDI.plugin"; do
	v="$(plutil -extract CFBundleShortVersionString raw "$b/Contents/Info.plist")"
	[ "$v" = "$BUNDLE_VERSION" ] || fail "$(basename "$b") version $v, expected $BUNDLE_VERSION"
done
v="$("$P/Library/Application Support/Ozzy/OzzyDaemon" --version)"
[ "$v" = "$VERSION" ] || fail "OzzyDaemon --version is $v, expected $VERSION"

for b in "$P/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver" "$P/Library/Audio/MIDI Drivers/OzzyMIDI.plugin" "$P/Library/Application Support/Ozzy/OzzyDaemon"; do
	codesign --verify --strict "$b" || fail "$(basename "$b") does not verify"
	sig="$(codesign -dv "$b" 2>&1)"
	grep -qx 'Signature=adhoc' <<<"$sig" || fail "$(basename "$b") is not ad-hoc signed"
	if [ -d "$b" ]; then
		exe="$b/Contents/MacOS/$(basename "${b%.*}")"
	else
		exe="$b"
	fi
	[ "$(lipo -archs "$exe")" = "arm64" ] || fail "$(basename "$b") is not arm64 only"
done

plutil -lint "$P/Library/LaunchAgents/com.ozzy.daemon.plist" >/dev/null || fail "LaunchAgent plist invalid"
grep -q -- '--log-file' "$P/Library/LaunchAgents/com.ozzy.daemon.plist" || fail "LaunchAgent lacks --log-file"
# The agent runs as every user who logs in, so its binary must sit in a directory only root can write.
grep -q '<string>/Library/Application Support/Ozzy/OzzyDaemon</string>' "$P/Library/LaunchAgents/com.ozzy.daemon.plist" \
	|| fail "LaunchAgent does not run /Library/Application Support/Ozzy/OzzyDaemon"
bom="$(lsbom -d -p MUGf "$COMP/Bom")"
grep -qx $'drwxr-xr-x \troot\twheel\t./Library/Application Support/Ozzy' <<<"$bom" \
	|| fail "/Library/Application Support/Ozzy is not root:wheel 755"
for s in preinstall postinstall uninstall.sh; do
	[ -x "$COMP/Scripts/$s" ] || fail "script $s missing or not executable"
done
[ -x "$P/Library/Application Support/Ozzy/uninstall.sh" ] || fail "installed uninstall.sh not executable"
echo "package checks passed: $PKG"
