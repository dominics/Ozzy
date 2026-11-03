#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# The version stamp and the options that exit before any device matching: a build with a test
# version prints it for --version and carries it in its embedded Info.plist; --list names the
# default model; --log-file writes, rotates and reports a failure (--log-file then an unknown
# option: the usage text goes to the log, and the daemon exits). Never starts the daemon proper,
# so no device is touched.
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

# --log-file: always followed by --bogus, so the daemon exits at the usage text (status 2)
FAKE_HOME="$(mktemp -d "$DIR/$OUT/home.XXXXXX")"
mkdir -p "$FAKE_HOME/Library"
LOG="$FAKE_HOME/Library/Logs/OzzyDaemon.log"
STAMP='^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3} '
log_run() {
	local status=0
	HOME="$1" "$BIN" --log-file --bogus 2>"$DIR/$OUT/stderr.txt" || status=$?
	[ "$status" = 2 ] || { echo "FAIL: --log-file --bogus exited $status"; fail=1; }
}

log_run "$FAKE_HOME"
grep -Eq "${STAMP}usage: ozzy-daemon" "$LOG" 2>/dev/null ||
	{ echo "FAIL: --log-file: no timestamped usage line in $LOG"; fail=1; }

# Past the cap at the start: moved to .1, a fresh file
rm -f "$LOG"
head -c $((6 * 1024 * 1024)) /dev/zero >"$LOG"
log_run "$FAKE_HOME"
[ "$(wc -c <"$LOG.1")" -ge $((6 * 1024 * 1024)) ] && [ "$(wc -c <"$LOG")" -lt 4096 ] &&
	grep -Eq "${STAMP}usage: ozzy-daemon" "$LOG" ||
	{ echo "FAIL: --log-file: a 6 MB log was not rotated at start"; ls -l "$FAKE_HOME/Library/Logs"; fail=1; }

# Reaching the cap while running: rotated between lines (the first line into .1, the rest after)
rm -f "$LOG" "$LOG.1"
head -c $((5 * 1024 * 1024 - 1)) /dev/zero >"$LOG"
log_run "$FAKE_HOME"
grep -aq "usage: ozzy-daemon" "$LOG.1" 2>/dev/null && ! grep -q "usage: ozzy-daemon" "$LOG" &&
	grep -Eq "${STAMP} +--device KEY" "$LOG" ||
	{ echo "FAIL: --log-file: not rotated when the log reached the cap"; ls -l "$FAKE_HOME/Library/Logs"; fail=1; }

# No log file to be had: said on stderr, which is left as it was (the usage text there)
log_run "$FAKE_HOME/missing"
grep -q "no log file: creating the folder of $FAKE_HOME/missing/Library/Logs/OzzyDaemon.log" "$DIR/$OUT/stderr.txt" &&
	grep -q "usage: ozzy-daemon" "$DIR/$OUT/stderr.txt" ||
	{ echo "FAIL: --log-file: a failure was not reported on stderr:"; cat "$DIR/$OUT/stderr.txt"; fail=1; }

rm -rf "${DIR:?}/$OUT"
[ "$fail" = 0 ] && echo "cli: ok"
exit "$fail"
