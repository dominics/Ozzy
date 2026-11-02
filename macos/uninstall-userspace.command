#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Remove the SIP-free Ozzy install: LaunchAgent, daemon, HAL and MIDI plug-ins.
set -euo pipefail
[ "$(id -u)" -ne 0 ] || { echo "Run this as your user, not with sudo; it asks for sudo itself."; exit 1; }
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -d "/Library/Extensions/OzzyKext.kext" ]; then
	echo "This machine has the kext install; its HAL and MIDI plug-ins belong to that. Run ./uninstall.command instead."
	exit 1
fi
sudo "$DIR/packaging/uninstall.sh"
