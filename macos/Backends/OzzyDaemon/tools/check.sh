#!/usr/bin/env bash
# fake-daemon and shm-client together (make check): the segment as a client sees it, the
# mailbox, MIDI through a loop, and a hung and a crashed daemon as the plug-ins must see them.
set -euo pipefail
cd "$(dirname "$0")"
B=${1:-build}
# A location of our own, so parallel runs don't share a segment
LOC=$(printf '0e%06x' $(( ($$ * 7919 + RANDOM) & 0xffffff )))
FAKE=
LOG=$(mktemp -t ozzy-fake-daemon)

cleanup() {
	[ -n "$FAKE" ] && kill "$FAKE" 2>/dev/null && wait "$FAKE" 2>/dev/null || true
	rm -f "$LOG" "$LOG.backend"
}
trap cleanup EXIT

fail() {
	echo "tools: FAILED: $*"
	echo "--- fake-daemon log"
	cat "$LOG"
	exit 1
}

client() { "$B/shm-client" --location "$LOC" "$@"; }
backend() { "$B/backend-client" "$LOC" "$@"; }

start_fake() {
	"$B/fake-daemon" --location "$LOC" "$@" >> "$LOG" 2>&1 &
	FAKE=$!
	# Its own start line: a stale segment left by a crash reads as ready too
	for _ in $(seq 100); do
		grep -q "(pid $FAKE)" "$LOG" && client --wait-ready 10 > /dev/null 2>&1 && return 0
		sleep 0.05
	done
	fail "fake-daemon $* didn't come up"
}

# 1. A live segment: what it publishes, the mailbox, MIDI
start_fake --midi-loop
client --info --alive 1000 > /dev/null || fail "not alive"
client --expect-rate 96000 > /dev/null || fail "not at the CMS 600-3's start rate"
client --set-rate 48000 --expect-rate 48000 --set-rate 44100 --expect-rate 44100 > /dev/null || fail "SET_RATE"
client --set-rate 12345 > /dev/null 2>&1 && fail "SET_RATE took a rate the model doesn't have"
client --midi-loop 903c40803c00 > /dev/null || fail "MIDI didn't come back through the loop"
echo "tools: live segment ok (info, heartbeat, SET_RATE, MIDI loop)"
backend --alive --set-rate 48000 --midi-loop 903c40 > /dev/null || fail "backend: live segment"
backend --gone 1500 > /dev/null && fail "backend: a live daemon counted as gone"
echo "tools: backend ok on a live segment (alive over 1.5 s, SET_RATE, MIDI loop)"

# The plug-ins' backend keeps its mapping while the daemon hangs
backend --alive --gone 3000 > "$LOG.backend" &
BC=$!
sleep 0.6

# 2. A hung daemon: the segment stays as it was, only the heartbeat stops
kill -USR1 "$FAKE"
sleep 0.1
client --dead 1200 > /dev/null || fail "a hung daemon still looks alive"
wait "$BC" || fail "backend: a hung daemon still counted as alive ($(cat "$LOG.backend"))"
kill "$FAKE"; wait "$FAKE" 2>/dev/null || true; FAKE=
client --info > /dev/null 2>&1 && fail "the segment is still there after SIGTERM"
backend --open-fails > /dev/null || fail "backend: opened a removed segment"
echo "tools: hung daemon ok (heartbeat stops, backend sees it gone; SIGTERM removes the segment)"

# 3. A crashed daemon: the segment is left behind, magic and all; the next one replaces it
start_fake --crash-after 2
backend --alive --gone 4000 > "$LOG.backend" || fail "backend: a crashed daemon still counted as alive ($(cat "$LOG.backend"))"
wait "$FAKE" 2>/dev/null || true; FAKE=
client --dead 1200 > /dev/null || fail "a crashed daemon's segment looks alive"
backend --gone 1500 > /dev/null || fail "backend: a stale segment opened as alive"
start_fake
grep -q "replaced a stale segment" "$LOG" || fail "the next daemon didn't replace the stale segment"
client --alive 1000 > /dev/null || fail "the replacement isn't alive"
backend --alive > /dev/null || fail "backend: the replacement isn't alive"
echo "tools: crashed daemon ok (stale segment, backend sees it gone, replaced by the next daemon)"
# 4. A restart as slow as the real handshake stops the heartbeat; the backend mustn't drop the device
kill "$FAKE"; wait "$FAKE" 2>/dev/null || true; FAKE=
start_fake --restart-delay 1500
backend --set-rate-watched 48000 > "$LOG.backend" || fail "backend: a slow restart counted as a dead daemon ($(cat "$LOG.backend"))"
echo "tools: slow restart ok (1.5 s, heartbeat stopped, the backend kept the device)"
echo "all tools tests passed"
