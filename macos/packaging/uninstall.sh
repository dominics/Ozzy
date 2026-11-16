#!/bin/bash
# SPDX-License-Identifier: MIT
# Removes the Ozzy userspace driver for the Dynacord CMS 600-3. Run as root.
# --for-upgrade (from preinstall) stops the daemon and removes the binaries
# the package is about to replace, and leaves the receipt and LaunchDaemon
# plist alone. Either way it removes a 0.4.x install: the per-user
# LaunchAgent and its daemon under /Library/Application Support/Ozzy.
set -euo pipefail
LABEL=com.ozzy.daemon
FOR_UPGRADE=0
[ "${1:-}" = "--for-upgrade" ] && FOR_UPGRADE=1
if [ "$(id -u)" -ne 0 ]; then
	echo "Run this with sudo." >&2
	exit 1
fi

# Wait for launchd to have stopped job $1
wait_gone() {
	for _ in $(seq 50); do
		launchctl print "$1" >/dev/null 2>&1 || break
		sleep 0.1
	done
}

# 0.4.x ran the daemon as a LaunchAgent of each logged-in user, under the same label: it has to be
# gone before the system daemon starts, or the two fight over the device. One loginwindow process
# runs per logged-in GUI user; uid 0 is the login window itself.
for uid in $(ps -axo uid=,comm= | awk '$1 != 0 && $2 ~ /\/loginwindow$/ {print $1}' | sort -u); do
	launchctl bootout "gui/$uid/$LABEL" 2>/dev/null || true
	wait_gone "gui/$uid/$LABEL"
done
rm -f "/Library/LaunchAgents/$LABEL.plist" "/Library/Application Support/Ozzy/OzzyDaemon"

launchctl bootout "system/$LABEL" 2>/dev/null || true
wait_gone "system/$LABEL"

# Removed on upgrade too: installer compares bundle versions before replacing
# a bundle, and an older-looking package would otherwise keep the old one.
rm -f /usr/local/bin/OzzyDaemon
rm -rf /Library/Audio/Plug-Ins/HAL/OzzyHAL.driver "/Library/Audio/MIDI Drivers/OzzyMIDI.plugin"
if [ "$FOR_UPGRADE" -eq 1 ]; then
	exit 0
fi
rm -f "/Library/LaunchDaemons/$LABEL.plist"
rm -f "/Library/Application Support/Ozzy/uninstall.sh"
rmdir "/Library/Application Support/Ozzy" 2>/dev/null || true
pkgutil --forget com.ozzy.userspace >/dev/null 2>&1 || true
# SIGTERM so coreaudiod shuts down in order (launchd starts it again on demand
# without the plug-ins); SIGKILL only if it is still there after 5 s.
pid="$(pgrep -x coreaudiod || true)"
if [ -n "$pid" ]; then
	kill -TERM "$pid" 2>/dev/null || true
	for _ in $(seq 50); do
		kill -0 "$pid" 2>/dev/null || break
		sleep 0.1
	done
	kill -KILL "$pid" 2>/dev/null || true
fi
killall MIDIServer 2>/dev/null || true
echo "Ozzy userspace driver removed."
