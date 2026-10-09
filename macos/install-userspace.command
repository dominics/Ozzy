#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Install Ozzy for the Dynacord CMS 600-3 without kernel extensions:
# the OzzyHAL plug-in plus (from Task 13) the OzzyDaemon LaunchAgent.
set -euo pipefail
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

sudo -v
sudo rm -rf "/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver"
sudo cp -R "$HAL_PATH" "/Library/Audio/Plug-Ins/HAL/"
sudo chown -R root:wheel "/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver"
sudo chmod -R 755 "/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver"
sudo killall -9 coreaudiod 2>/dev/null || true
echo "OzzyHAL installed; coreaudiod restarted."
