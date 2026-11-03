#!/usr/bin/env bash
# Release loopback run for the CMS 600-3 under OzzyDaemon: duplex audio at each rate with both MIDI
# ports looping during it, a 300 s run at 48 kHz, a minute idle, then the daemon's log since the start.
# Needs the mixer's loopback (outputs 1-2 and 3-4 into inputs 1-2) and MIDI OUT -> IN on both ports.
# usage: loopback.sh [outdir]   (builds duplex and midiport next to this script on first use)
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
out="${1:-loopback-$(date +%Y%m%d-%H%M%S)}"
log="$HOME/Library/Logs/OzzyDaemon.log"
lock=/tmp/ploytec-hw.lock

for t in duplex midiport; do
	if [ ! -x "$here/$t" ] || [ "$here/$t.swift" -nt "$here/$t" ]; then
		swiftc -O -o "$here/$t" "$here/$t.swift" || exit 1
	fi
done

if [ -e "$lock" ]; then
	echo "mixer in use: $(cat "$lock")"
	exit 1
fi
echo "loopback.sh $$ $(date +%T)" > "$lock"
trap 'rm -f "$lock"' EXIT

mkdir -p "$out"
start=$(wc -l < "$log")
for r in 44100 48000 96000; do
	echo "=== $r: duplex 30 s with MIDI on both ports"
	"$here/duplex" DYNACORD "$r" 30 "$out/d$r.wav" > "$out/d$r.out" 2>&1 &
	dp=$!
	sleep 6
	for _ in 1 2 3; do
		echo "  usb-midi: $("$here/midiport" loop USB-MIDI 2>&1 | tail -n 1)"
		echo "  sysctrl:  $("$here/midiport" sysex SystemCtrl 2>&1 | tail -n 1)"
		sleep 2
	done
	wait "$dp"
	tail -n 3 "$out/d$r.out"
	python3 "$here/analyse.py" "$out/d$r.wav"
	sleep 3
done

echo "=== 48000 long: 300 s"
"$here/duplex" DYNACORD 48000 300 "$out/long48.wav" > "$out/long48.out" 2>&1
tail -n 3 "$out/long48.out"
python3 "$here/analyse.py" "$out/long48.wav"

echo "=== idle 60 s after stop (watch for 0x86 recoveries)"
sleep 60

echo "=== daemon log since start"
tail -n +$((start + 1)) "$log" | grep -iE "recover|restart|watchdog|stall|0x86|lost|fail|error|timestamp|anchor" | head -n 40
echo "log lines added: $(($(wc -l < "$log") - start))"
ps -o pcpu=,comm= -p "$(pgrep -x coreaudiod)"
