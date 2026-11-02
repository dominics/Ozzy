#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# The version stamp and the options that exit before any device matching: a build with a test
# version prints it for --version and carries it in its embedded Info.plist; --list names the
# default model. Never starts the daemon proper, so no device is touched.
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT=build/clitest
VERSION=9.8.7-rc.1+test
make -s -C "$DIR" BUILD="$OUT" OZZY_VERSION="$VERSION" "$OUT/ozzy-daemon"
BIN="$DIR/$OUT/ozzy-daemon"
fail=0

got="$("$BIN" --version)"
[ "$got" = "$VERSION" ] || { echo "FAIL: --version printed '$got'"; fail=1; }

plist="$(launchctl plist __TEXT,__info_plist "$BIN")"
grep -q '"CFBundleShortVersionString" = "9.8.7-rc.1+test"' <<<"$plist" ||
	{ echo "FAIL: CFBundleShortVersionString missing:"; echo "$plist"; fail=1; }
grep -q '"CFBundleVersion" = "9.8.7"' <<<"$plist" || { echo "FAIL: CFBundleVersion missing:"; echo "$plist"; fail=1; }
grep -q '"CFBundleIdentifier" = "com.ozzy.daemon"' <<<"$plist" ||
	{ echo "FAIL: CFBundleIdentifier missing:"; echo "$plist"; fail=1; }

"$BIN" --list | grep -q '^DynacordCMS600 ' || { echo "FAIL: --list lacks DynacordCMS600"; fail=1; }

if "$BIN" --bogus 2>/dev/null; then
	echo "FAIL: an unknown option was accepted"
	fail=1
fi

rm -rf "${DIR:?}/$OUT"
[ "$fail" = 0 ] && echo "cli: ok"
exit "$fail"
