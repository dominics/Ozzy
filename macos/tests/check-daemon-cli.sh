#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Checks OzzyDaemon's --version and --log-file without a mixer attached.
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DAEMON_DIR="$DIR/Backends/OzzyDaemon"
make -s -C "$DAEMON_DIR" clean
make -s -C "$DAEMON_DIR" build/OzzyDaemon OZZY_VERSION=9.8.7-test
BIN="$DAEMON_DIR/build/OzzyDaemon"

got="$("$BIN" --version)"
[ "$got" = "9.8.7-test" ] || { echo "FAIL: --version printed '$got'"; exit 1; }

FAKE_HOME="$(mktemp -d "$DAEMON_DIR/build/home.XXXXXX")"
HOME="$FAKE_HOME" "$BIN" --log-file &
pid=$!
sleep 1
kill -TERM "$pid"
wait "$pid" || true
LOG="$FAKE_HOME/Library/Logs/OzzyDaemon.log"
[ -f "$LOG" ] || { echo "FAIL: no $LOG"; exit 1; }
grep -q '^OzzyDaemon 9.8.7-test starting$' "$LOG" || { echo "FAIL: version line missing:"; cat "$LOG"; exit 1; }
rm -rf "$FAKE_HOME"
make -s -C "$DAEMON_DIR" clean
echo "daemon CLI checks passed"
