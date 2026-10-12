#!/bin/bash

usage() {
    echo "usage: $0 [--yes] [--reboot | --no-reboot]"
    echo "  --yes        don't ask for confirmation"
    echo "  --reboot     reboot when done without asking"
    echo "  --no-reboot  don't reboot and don't ask"
}

ASSUME_YES=false
REBOOT=ask
NEED_REBOOT=false
AUXKC_PENDING=false
BUILD_VERSION=$(sw_vers -buildVersion)
for arg in "$@"; do
    case "$arg" in
        -y|--yes) ASSUME_YES=true ;;
        --reboot) REBOOT=yes ;;
        --no-reboot) REBOOT=no ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

# Without a terminal a prompt reads nothing, which would quietly mean "no".
if [ ! -t 0 ]; then
    if ! $ASSUME_YES; then
        echo "No terminal to confirm on; pass --yes to uninstall unattended." >&2
        exit 1
    fi
    [ "$REBOOT" = ask ] && REBOOT=no
fi

[ -t 1 ] && clear
echo "======================================================"
echo "        Ozzy Audio Engine - Uninstaller"
echo "======================================================"
echo ""
echo "This will remove ALL Ozzy components:"
echo "  • Kernel Extension (if installed)"
echo "  • DriverKit Extension (if installed)"
echo "  • HAL Audio Driver (if installed)"
echo "  • MIDI Driver (if installed)"
echo "  • All LaunchDaemons (if installed)"
echo "  • All binaries and daemons (if installed)"
echo ""

echo "🔑 Administrator privileges required..."
sudo -v || exit 1
echo ""

if ! $ASSUME_YES; then
    read -p "Are you sure? (y/N): " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 0
    fi
    echo ""
fi

# Unload and remove new Kext LaunchDaemon
if [ -f "/Library/LaunchDaemons/com.ozzy.kext.load.plist" ]; then
    echo "🗑️  Unloading Kext LaunchDaemon..."
    sudo launchctl unload /Library/LaunchDaemons/com.ozzy.kext.load.plist 2>/dev/null || true
    sudo rm /Library/LaunchDaemons/com.ozzy.kext.load.plist
    echo "✅ Kext LaunchDaemon removed"
fi

# Unload and remove old Ozzy Daemon + LaunchDaemon (from install-mac.command)
if [ -f "/Library/LaunchDaemons/Ozzy.plist" ]; then
    echo "🗑️  Unloading Ozzy Daemon..."
    sudo launchctl unload /Library/LaunchDaemons/Ozzy.plist 2>/dev/null || true
    sudo rm /Library/LaunchDaemons/Ozzy.plist
    echo "✅ Ozzy Daemon LaunchDaemon removed"
fi

if [ -f "/usr/local/bin/Ozzy" ]; then
    echo "🗑️  Removing Ozzy binary..."
    sudo rm -f /usr/local/bin/Ozzy
    echo "✅ Ozzy binary removed"
fi

# Unload and remove Kernel Extension
if kmutil showloaded 2>/dev/null | grep -q "OzzyKext"; then
    echo "🔄 Unloading Kernel Extension..."
    sudo killall -9 coreaudiod 2>/dev/null || true
    sleep 1
    sudo kextunload -b "com.ozzy.kext.OzzyKext" 2>/dev/null || true
    sleep 1
    if kmutil showloaded 2>/dev/null | grep -q "OzzyKext"; then
        echo "⚠️  Kext is still loaded (in use); it stays loaded until a reboot"
        NEED_REBOOT=true
    else
        echo "✅ Kext unloaded"
    fi
fi

if [ -d "/Library/Extensions/OzzyKext.kext" ]; then
    echo "🗑️  Removing Kernel Extension..."
    sudo rm -rf /Library/Extensions/OzzyKext.kext
    sudo touch /Library/Extensions
    echo "✅ Kext removed"
fi

# Deleting the bundle doesn't take the kext out of the auxiliary kernel
# collection, which macOS loads at boot, so it keeps loading until that
# collection is rebuilt. kmutil rebuild drops kexts whose bundles are gone,
# but needs a Kernel Debug Kit for the running build and a GUI approval.
if kmutil inspect -V release --no-header 2>/dev/null | grep -q 'com\.ozzy\.kext\.OzzyKext'; then
    NEED_REBOOT=true
    AUXKC_PENDING=true
    KDK_DIR=$(ls -d /Library/Developer/KDKs/KDK_*_"$BUILD_VERSION".kdk 2>/dev/null | head -n 1)
    if [ -n "$KDK_DIR" ] && [ -t 0 ]; then
        echo "🔄 Rebuilding the kernel extension collection without the kext..."
        echo "   Approve the dialog that follows; it times out in 60 seconds."
        if sudo kmutil rebuild; then
            echo "✅ Collection rebuilt; the kext is gone after a reboot"
            AUXKC_PENDING=false
        else
            echo "❌ kmutil rebuild failed (see its message above)"
        fi
    fi
fi

# Remove HAL Driver
if [ -d "/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver" ]; then
    echo "🗑️  Removing HAL Driver..."
    sudo rm -rf "/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver"
    echo "✅ HAL Driver removed"
fi

# Remove MIDI Driver
if [ -d "/Library/Audio/MIDI Drivers/OzzyMIDI.plugin" ]; then
    echo "🗑️  Removing MIDI Driver..."
    sudo rm -rf "/Library/Audio/MIDI Drivers/OzzyMIDI.plugin"
    sudo touch "/Library/Audio/MIDI Drivers"
    echo "✅ MIDI Driver removed"
fi

# Remove DriverKit Extension (if installed)
if [ -d "/Applications/Ploytec Driver Extension.app" ]; then
    echo "🗑️  Removing DriverKit Extension..."
    sudo rm -rf "/Applications/Ploytec Driver Extension.app"
    echo "✅ DriverKit Extension removed"
fi

# Remove future Userspace Daemon components (if they exist)
if [ -f "/usr/local/bin/OzzyDaemon" ]; then
    echo "🗑️  Removing Userspace Daemon..."
    sudo rm -f /usr/local/bin/OzzyDaemon
    echo "✅ Userspace Daemon removed"
fi

# Restart audio services
echo "🔄 Restarting audio services..."
sudo killall coreaudiod 2>/dev/null || true
sudo killall MIDIServer 2>/dev/null || true

echo ""
echo "======================================================"
echo "          ✅ Uninstall Complete!"
echo "======================================================"
echo ""
echo "All Ozzy components have been removed from your system."
echo ""
if $AUXKC_PENDING; then
    echo "⚠️  The kext is still in the kernel extension collection, so it"
    echo "   loads again at every boot until that collection is rebuilt:"
    echo "   1. Install the Kernel Debug Kit for build $BUILD_VERSION"
    echo "      (https://developer.apple.com/download/all)"
    echo "   2. Run: sudo kmutil rebuild   and approve the dialog"
    echo "   3. Reboot"
    echo ""
elif $NEED_REBOOT; then
    echo "⚠️  The kext stays loaded until you reboot."
    echo ""
else
    echo "Note: You may want to reboot to ensure all services are"
    echo "fully cleaned up."
    echo ""
fi

if [ "$REBOOT" = ask ]; then
    if $NEED_REBOOT && ! $AUXKC_PENDING; then
        read -p "Reboot now? (Y/n): " -n 1 -r
        echo
        [[ ! $REPLY =~ ^[Nn]$ ]] && REBOOT=yes
    else
        read -p "Reboot now? (y/N): " -n 1 -r
        echo
        [[ $REPLY =~ ^[Yy]$ ]] && REBOOT=yes
    fi
fi
if [ "$REBOOT" = yes ]; then
    echo "🔄 Rebooting in 5 seconds... (Ctrl+C to cancel)"
    sleep 5
    sudo reboot
fi