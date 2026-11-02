#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Builds Ozzy-CMS600-<version>.pkg: OzzyHAL, OzzyMIDI and the OzzyDaemon
# LaunchAgent, ad-hoc signed, with arm64 and x86_64 slices. No kext.
# Usage: build-pkg.sh [VERSION]
set -euo pipefail
PKGDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MACOS="$(dirname "$PKGDIR")"
cd "$MACOS"

if [ $# -ge 1 ]; then
	VERSION="$1"
elif tag="$(git describe --tags --match 'v*' 2>/dev/null)"; then
	VERSION="${tag#v}"
elif sha="$(git rev-parse --short HEAD 2>/dev/null)"; then
	VERSION="0.0.0-dev+$sha"
else
	VERSION="0.0.0-dev"
fi
BUNDLE_VERSION="${VERSION%%[-+]*}"
BUILD_NUMBER="$(git rev-list --count HEAD 2>/dev/null || echo 0)"
OUT="$MACOS/build/pkg"
WORK="$OUT/work"
rm -rf "$OUT"
mkdir -p "$WORK/root" "$WORK/scripts"
echo "Building Ozzy-CMS600 $VERSION (bundle $BUNDLE_VERSION, build $BUILD_NUMBER)"

# The build settings install.command uses, for both architectures and unsigned (signed below).
# 12.0 is the oldest deployment target Xcode 26 builds for.
xc() {
	xcodebuild -quiet -project Ozzy.xcodeproj -scheme "$1" -configuration Release -derivedDataPath "$WORK/dd" \
		-destination 'generic/platform=macOS' ARCHS="arm64 x86_64" ONLY_ACTIVE_ARCH=NO CODE_SIGNING_ALLOWED=NO \
		MACOSX_DEPLOYMENT_TARGET=12.0 \
		GCC_PREPROCESSOR_DEFINITIONS="\$(inherited) OZZY_GIT_VERSION=\\\"$VERSION\\\"" \
		OTHER_LDFLAGS="$2" build
}
xc OzzyHAL "-framework IOKit -framework CoreFoundation"
xc OzzyMIDI "-framework IOKit -framework CoreFoundation -framework CoreMIDI"
PRODUCTS="$WORK/dd/Build/Products/Release"

make -s -C Backends/OzzyDaemon clean
make -s -C Backends/OzzyDaemon build/ozzy-daemon UNIVERSAL=1 OZZY_VERSION="$VERSION"

R="$WORK/root"
mkdir -p "$R/Library/Audio/Plug-Ins/HAL" "$R/Library/Audio/MIDI Drivers" "$R/Library/LaunchAgents" \
	"$R/Library/Application Support/Ozzy"
cp -R "$PRODUCTS/OzzyHAL.driver" "$R/Library/Audio/Plug-Ins/HAL/"
cp -R "$PRODUCTS/OzzyMIDI.plugin" "$R/Library/Audio/MIDI Drivers/"
cp Backends/OzzyDaemon/build/ozzy-daemon "$R/Library/Application Support/Ozzy/OzzyDaemon"
cp packaging/com.ozzy.daemon.plist "$R/Library/LaunchAgents/"
cp packaging/uninstall.sh "$R/Library/Application Support/Ozzy/"
chmod 755 "$R/Library/Application Support/Ozzy" "$R/Library/Application Support/Ozzy/OzzyDaemon" \
	"$R/Library/Application Support/Ozzy/uninstall.sh"
chmod 644 "$R/Library/LaunchAgents/com.ozzy.daemon.plist"

# The plug-ins' Info.plists carry a fixed version (or none): stamp the package's into the built copies
for b in "$R/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver" "$R/Library/Audio/MIDI Drivers/OzzyMIDI.plugin"; do
	plutil -replace CFBundleShortVersionString -string "$BUNDLE_VERSION" "$b/Contents/Info.plist"
	plutil -replace CFBundleVersion -string "$BUILD_NUMBER" "$b/Contents/Info.plist"
done
for b in "$R/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver" "$R/Library/Audio/MIDI Drivers/OzzyMIDI.plugin" \
	"$R/Library/Application Support/Ozzy/OzzyDaemon"; do
	codesign --force --sign - "$b"
done
make -s -C Backends/OzzyDaemon clean

cp packaging/scripts/preinstall packaging/scripts/postinstall packaging/uninstall.sh "$WORK/scripts/"
chmod 755 "$WORK/scripts/"*

# Without these, installer "upgrades" a same-ID bundle it finds elsewhere on
# disk, and skips a bundle whose installed copy has a higher version.
pkgbuild --analyze --root "$R" "$WORK/component.plist"
n="$(plutil -convert json -o - "$WORK/component.plist" | python3 -c 'import json,sys; print(len(json.load(sys.stdin)))')"
for ((i = 0; i < n; i++)); do
	/usr/libexec/PlistBuddy -c "Delete :$i:BundleIsRelocatable" "$WORK/component.plist" 2>/dev/null || true
	/usr/libexec/PlistBuddy -c "Add :$i:BundleIsRelocatable bool false" \
		-c "Set :$i:BundleIsVersionChecked false" "$WORK/component.plist"
done

pkgbuild --root "$R" --component-plist "$WORK/component.plist" --scripts "$WORK/scripts" \
	--identifier com.ozzy.userspace --version "$VERSION" --install-location / --ownership recommended \
	"$WORK/ozzy-userspace.pkg"
sed "s/@VERSION@/$VERSION/" packaging/distribution.xml > "$WORK/distribution.xml"
NAME="Ozzy-CMS600-$VERSION.pkg"
productbuild --distribution "$WORK/distribution.xml" --package-path "$WORK" "$OUT/$NAME"
(cd "$OUT" && shasum -a 256 "$NAME" > "$NAME.sha256")
echo "$VERSION" > "$OUT/VERSION"
rm -rf "$WORK"
echo "Built $OUT/$NAME"
