#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Install Ozzy for the Dynacord CMS 600-3 without kernel extensions:
# the OzzyHAL and OzzyMIDI plug-ins plus the OzzyDaemon LaunchAgent.
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

IDENTITY_LINE="$(security find-identity -v -p codesigning | grep "Apple Development" | head -n 1 || true)"
[ -n "$IDENTITY_LINE" ] || IDENTITY_LINE="$(security find-identity -v -p codesigning | grep "Developer ID Application" | head -n 1 || true)"
if [ -z "$IDENTITY_LINE" ]; then
	echo "No Apple Development or Developer ID signing identity found in the keychain."
	exit 1
fi
CODE_SIGN_IDENTITY=$(echo "$IDENTITY_LINE" | awk '{print $2}')

BUILD_DIR="$DIR/build-userspace"
rm -rf "$BUILD_DIR"
xcodebuild -project Ozzy.xcodeproj -scheme OzzyHAL -configuration Release \
	-derivedDataPath "$BUILD_DIR/dd" OTHER_LDFLAGS="-framework IOKit -framework CoreFoundation -framework SystemConfiguration" \
	CODE_SIGNING_ALLOWED=NO build
HAL_PATH="$BUILD_DIR/dd/Build/Products/Release/OzzyHAL.driver"
codesign --sign "$CODE_SIGN_IDENTITY" --force --options runtime "$HAL_PATH"

xcodebuild -project Ozzy.xcodeproj -scheme OzzyMIDI -configuration Release \
	-derivedDataPath "$BUILD_DIR/dd" OTHER_LDFLAGS="-framework IOKit -framework CoreFoundation -framework SystemConfiguration -framework CoreMIDI" \
	CODE_SIGNING_ALLOWED=NO build
MIDI_PATH="$BUILD_DIR/dd/Build/Products/Release/OzzyMIDI.plugin"
codesign --sign "$CODE_SIGN_IDENTITY" --force --options runtime "$MIDI_PATH"

sudo -v
sudo rm -rf "/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver"
sudo cp -R "$HAL_PATH" "/Library/Audio/Plug-Ins/HAL/"
sudo chown -R root:wheel "/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver"
sudo chmod -R 755 "/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver"

sudo rm -rf "/Library/Audio/MIDI Drivers/OzzyMIDI.plugin"
sudo cp -R "$MIDI_PATH" "/Library/Audio/MIDI Drivers/"
sudo chown -R root:wheel "/Library/Audio/MIDI Drivers/OzzyMIDI.plugin"
sudo chmod -R 755 "/Library/Audio/MIDI Drivers/OzzyMIDI.plugin"
sudo touch "/Library/Audio/MIDI Drivers"

sudo killall -9 coreaudiod 2>/dev/null || true
sudo killall MIDIServer 2>/dev/null || true
echo "OzzyHAL and OzzyMIDI installed; coreaudiod restarted."

make -C Backends/OzzyDaemon build/OzzyDaemon
codesign --sign "$CODE_SIGN_IDENTITY" --force --options runtime Backends/OzzyDaemon/build/OzzyDaemon
PLIST="$HOME/Library/LaunchAgents/com.ozzy.daemon.plist"
launchctl bootout "gui/$(id -u)/com.ozzy.daemon" 2>/dev/null || true
for _ in $(seq 50); do
	launchctl print "gui/$(id -u)/com.ozzy.daemon" >/dev/null 2>&1 || break
	sleep 0.1
done
sudo mkdir -p /usr/local/bin
sudo rm -f /usr/local/bin/OzzyDaemon
sudo cp Backends/OzzyDaemon/build/OzzyDaemon /usr/local/bin/OzzyDaemon
sudo chmod 755 /usr/local/bin/OzzyDaemon
mkdir -p "$HOME/Library/LaunchAgents" "$HOME/Library/Logs"
sed "s|@HOME@|$HOME|g" Backends/OzzyDaemon/com.ozzy.daemon.plist > "$PLIST"
launchctl bootstrap "gui/$(id -u)" "$PLIST"
echo "OzzyDaemon installed as a LaunchAgent for $(id -un)."
echo "Done. Power-cycle the CMS 600-3 with the USB cable connected if it doesn't appear."
