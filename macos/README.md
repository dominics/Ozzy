# Ozzy - macOS Audio Driver

Clean, modern kernel extension (kext) driver for Ploytec-based USB audio interfaces on macOS.

## 🚀 Quick Start

**Double-click** [install.command](install.command) to install everything.

The installer will:
- ✅ Check System Integrity Protection (SIP) status
- ✅ Compile the kernel extension
- ✅ Compile HAL and MIDI drivers
- ✅ Install everything to system directories
- ✅ Configure auto-loading at boot
- ✅ Start the drivers immediately (a first install on Apple Silicon needs approval in System Settings > Privacy & Security and a reboot first)

## 🗑️ Uninstall

**Double-click** [uninstall.command](uninstall.command) to remove all components.

## 📋 Requirements

- **macOS 11+** (Big Sur or later)
- **Apple Silicon or Intel** (Universal binary)
- **SIP modification required**: Run `csrutil enable --without kext` in Recovery Mode
- **Apple Developer certificate** in Keychain (free Apple ID works)

## 🏗️ Architecture

This driver uses a **shared memory model** for zero-copy audio streaming:

```
┌─────────────────┐
│  Audio Apps     │
│  (Logic, etc)   │
└────────┬────────┘
         │ CoreAudio
┌────────▼────────┐      Shared Memory      ┌─────────────┐
│   OzzyHAL       │◄────────────────────────►│  OzzyKext   │
│  (HAL Driver)   │      Ring Buffers        │   (Kernel)  │
└─────────────────┘                          └──────┬──────┘
                                                    │ USB
                                             ┌──────▼──────┐
                                             │  Hardware   │
                                             │  (Xone:DB4) │
                                             └─────────────┘
```

### Components

- **OzzyKext** - Kernel extension handling USB communication
- **OzzyHAL** - CoreAudio HAL driver for audio I/O
- **OzzyMIDI** - CoreMIDI driver for MIDI I/O

### Supported Devices

| Device | Channels | Sample Rates | Status |
|--------|----------|--------------|--------|
| **Allen & Heath Xone:DB4** | 8×8 | 44.1/48/96 kHz | ✅ Perfect |
| **Allen & Heath Xone:DB2** | 8×8 | 44.1/48/96 kHz | ✅ Perfect |
| **Allen & Heath Xone:DX** | 8×8 | 44.1/48/96 kHz | ✅ Perfect |
| **Allen & Heath Xone:4D** | 8×8 | 44.1/48/96 kHz | ✅ Perfect |
| **Dynacord CMS 600-3** | 4x4 | 44.1/48/96 kHz | 2 MIDI ports |

### Dynacord CMS 600-3

- **SIP and security**: needs SIP with the kext exception (`csrutil enable --without kext` in Recovery). On Apple Silicon it also needs Reduced Security with user-managed kernel extensions (Startup Security Utility), and approval in System Settings > Privacy & Security after the first install, followed by a reboot.
- **Power-cycle quirk**: the mixer only enumerates if it is powered on with the USB cable already plugged in. If it does not show up, power it off, plug in the cable, then power it on.
- **Sample rate**: 44.1, 48 or 96 kHz, changed in Audio MIDI Setup.
- **MIDI**: two ports, "USB-MIDI" (the DIN sockets) and "SystemCtrl" (FX control). MIDI only moves while audio is streaming.
- **Latency**: output is queued as 8 x 3 ms OUT transfers (24 ms) with a 40 ms output safety offset.
- **Logs**: `log stream --predicate 'eventMessage CONTAINS "[OzzyKext]" OR eventMessage CONTAINS "[Dynacord]" OR eventMessage CONTAINS "[OzzyHAL]"'`
- **Without SIP changes**: a userspace alternative is planned separately.

## 🔧 Manual Build

If you prefer to build manually:

```bash
cd macos
xcodebuild -project Ozzy.xcodeproj -scheme OzzyHAL -configuration Release
xcodebuild -project Ozzy.xcodeproj -scheme OzzyMIDI -configuration Release
```

The build system will compile the kext inline during installation.

## 📖 Documentation

- [OzzyCore/](OzzyCore/) - Device-agnostic audio engine base classes
- [Backends/OzzyKext/](Backends/OzzyKext/) - Kernel extension implementation  
- [Devices/Ploytec/](Devices/Ploytec/) - Ploytec protocol implementation
- [Shared/](Shared/) - Shared memory structures and logging

## 🐛 Troubleshooting

**Kext won't load?**
- Verify SIP: `csrutil status` should show "Kext Signing: disabled"
- Check logs: `log stream --predicate 'sender CONTAINS "Ozzy"'`

**No audio device showing?**
- Ensure hardware is connected
- Restart CoreAudio: `sudo killall coreaudiod`
- Check Audio MIDI Setup

**Installation fails?**
- Make sure you have an Apple Developer certificate in Keychain
- Try running from Terminal: `./install.command`

## 📄 License

See [LICENSE](../LICENSE) in repository root.
