#!/bin/bash
# SPDX-License-Identifier: MIT
# Removes the Ozzy userspace driver for the Dynacord CMS 600-3. Run as root.
# --for-upgrade (from preinstall) stops the daemons and removes the binaries
# the package is about to replace, and leaves the receipt and system
# LaunchAgent alone.
set -euo pipefail
LABEL=com.ozzy.daemon
FOR_UPGRADE=0
[ "${1:-}" = "--for-upgrade" ] && FOR_UPGRADE=1
if [ "$(id -u)" -ne 0 ]; then
	echo "Run this with sudo." >&2
	exit 1
fi

# One loginwindow process runs per logged-in GUI user; uid 0 is the login window itself.
for uid in $(ps -axo uid=,comm= | awk '$1 != 0 && $2 ~ /\/loginwindow$/ {print $1}' | sort -u); do
	launchctl bootout "gui/$uid/$LABEL" 2>/dev/null || true
	for _ in $(seq 50); do
		launchctl print "gui/$uid/$LABEL" >/dev/null 2>&1 || break
		sleep 0.1
	done
done

# Removed on upgrade too: installer compares bundle versions before replacing
# a bundle, and an older-looking package would otherwise keep the old one.
rm -f "/Library/Application Support/Ozzy/OzzyDaemon"
rm -rf /Library/Audio/Plug-Ins/HAL/OzzyHAL.driver "/Library/Audio/MIDI Drivers/OzzyMIDI.plugin"
if [ "$FOR_UPGRADE" -eq 1 ]; then
	exit 0
fi
rm -f "/Library/LaunchAgents/$LABEL.plist"
rm -f "/Library/Application Support/Ozzy/uninstall.sh"
rmdir "/Library/Application Support/Ozzy" 2>/dev/null || true
pkgutil --forget com.ozzy.userspace >/dev/null 2>&1 || true
killall -9 coreaudiod 2>/dev/null || true
killall MIDIServer 2>/dev/null || true
echo "Ozzy userspace driver removed."
