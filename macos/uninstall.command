#!/bin/bash

usage() {
    echo "usage: $0 [--yes] [--reboot | --no-reboot]"
    echo "  --yes        don't ask for confirmation"
    echo "  --reboot     reboot when done without asking"
    echo "  --no-reboot  don't reboot and don't ask"
}

ASSUME_YES=false
REBOOT=ask
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
    echo "✅ Kext unloaded"
fi

if [ -d "/Library/Extensions/OzzyKext.kext" ]; then
    echo "🗑️  Removing Kernel Extension..."
    sudo rm -rf /Library/Extensions/OzzyKext.kext
    sudo touch /Library/Extensions
    echo "✅ Kext removed"
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
echo "Note: You may want to reboot to ensure all services are"
echo "fully cleaned up."
echo ""

if [ "$REBOOT" = ask ]; then
    read -p "Reboot now? (y/N): " -n 1 -r
    echo
    [[ $REPLY =~ ^[Yy]$ ]] && REBOOT=yes
fi
if [ "$REBOOT" = yes ]; then
    echo "🔄 Rebooting in 5 seconds... (Ctrl+C to cancel)"
    sleep 5
    sudo reboot
fi