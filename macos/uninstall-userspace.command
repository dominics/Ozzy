#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Remove the SIP-free Ozzy install: LaunchAgent, daemon, HAL and MIDI plug-ins.
set -euo pipefail
[ "$(id -u)" -ne 0 ] || { echo "Run this as your user, not with sudo; it asks for sudo itself."; exit 1; }
if [ -d "/Library/Extensions/OzzyKext.kext" ]; then
	echo "This machine has the kext install; its HAL and MIDI plug-ins belong to that. Run ./uninstall.command instead."
	exit 1
fi
launchctl bootout "gui/$(id -u)/com.ozzy.daemon" 2>/dev/null || true
for _ in $(seq 50); do
	launchctl print "gui/$(id -u)/com.ozzy.daemon" >/dev/null 2>&1 || break
	sleep 0.1
done
rm -f "$HOME/Library/LaunchAgents/com.ozzy.daemon.plist"
sudo rm -f /usr/local/bin/OzzyDaemon
sudo rm -rf /Library/Audio/Plug-Ins/HAL/OzzyHAL.driver
sudo rm -rf "/Library/Audio/MIDI Drivers/OzzyMIDI.plugin"
sudo killall -9 coreaudiod 2>/dev/null || true
sudo killall MIDIServer 2>/dev/null || true
echo "Ozzy userspace driver removed."
