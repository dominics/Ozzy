#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build the Ozzy userspace driver package for the Dynacord CMS 600-3 from
# source and install it: OzzyHAL, OzzyMIDI and the OzzyDaemon LaunchAgent.
set -euo pipefail
[ "$(id -u)" -ne 0 ] || { echo "Run this as your user, not with sudo; it asks for sudo itself."; exit 1; }
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

if [ -d "/Library/Extensions/OzzyKext.kext" ] || [ -f "/Library/LaunchDaemons/com.ozzy.kext.load.plist" ]; then
	echo "The Ozzy kext driver is installed. Run ./uninstall.command and reboot first."
	exit 1
fi

if [ -f "/usr/local/bin/Ozzy" ] || [ -f "/Library/LaunchDaemons/Ozzy.plist" ]; then
	echo "A legacy Ozzy daemon install is present. Run ./uninstall.command and reboot first."
	exit 1
fi

packaging/build-pkg.sh
VERSION="$(cat build/pkg/VERSION)"
sudo installer -pkg "build/pkg/Ozzy-CMS600-$VERSION.pkg" -target /
echo "Ozzy $VERSION installed: OzzyHAL, OzzyMIDI and the OzzyDaemon LaunchAgent."
echo "Power-cycle the CMS 600-3 with the USB cable connected if it doesn't appear."
