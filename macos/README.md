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
- **Without SIP changes**: see [Userspace driver](#userspace-driver-dynacord-cms-600-3-sip-enabled) below.

## Userspace driver (Dynacord CMS 600-3, SIP enabled)

For the Dynacord CMS 600-3 there is a driver that needs no kernel extension and no SIP change. It is the same OzzyHAL and OzzyMIDI plug-ins, fed by `OzzyDaemon`, a userspace program that talks to the mixer through IOUSBLib and shares the audio and MIDI buffers with them over the `/ozzy_shared_mem` segment. The daemon runs as a LaunchAgent (`com.ozzy.daemon`) for the logged-in user and is installed as `/usr/local/bin/OzzyDaemon`.

Install (from `macos/`; it asks for `sudo`, builds and signs everything, installs OzzyHAL, OzzyMIDI and the daemon, and restarts `coreaudiod` and `MIDIServer`):

```bash
./install-userspace.command      # or option 3 of ./install.command on a fresh install
```

Uninstall:

```bash
./uninstall-userspace.command
```

Notes:

- **Power-cycle quirk**: the mixer only enumerates if it is powered on with the USB cable already plugged in. If it does not show up, power it off, plug in the cable, then power it on.
- **Logged-in user only**: the daemon runs only while you are logged in. If a second logged-in user's agent already holds the device, this one stays idle until the mixer is re-attached (known v1 limit: it does not retry while the device stays plugged in). Anything else that opens the mixer (for example `ploytec-play`) while the daemon is retrying also makes it wait for a replug, and while the daemon streams it holds the device, so other tools cannot open it.
- **Sleep**: the daemon holds no sleep assertion. When the Mac sleeps the USB session stops, and the daemon restarts it on wake (about half a second, with a logged error). For long recordings on battery use `caffeinate -i` or a power assertion from the recording app.
- **Logs**: `log stream --predicate 'subsystem == "OzzyDaemon" OR eventMessage CONTAINS "[OzzyHAL]"'` and `~/Library/Logs/OzzyDaemon.log`.
- **Latency**: output is queued as 8 x 3 ms OUT transfers (24 ms) with a 40 ms output safety offset. The measured output lead is 1152-1294 frames (24-27 ms) at 48 kHz and 2112-2684 frames (22-28 ms) at 96 kHz. Input has a 25 ms safety offset. Through the mixer's loopback, the part of the round trip that CoreAudio's timestamps do not cover is 86 frames at 44.1 and 48 kHz and 94-102 frames at 96 kHz (1-2 ms); the device reports a latency of 0 for now. With a 512-frame IO buffer an app hears its own output back about 88 ms later at 48 kHz.
- **Input sign-bit glitches (open)**: in some sessions after a sample rate change, the mixer sends the first byte of each 64-byte input frame one frame late in about 8% of frames. That byte holds the sign bits of inputs 1 and 3, so quiet signals on them get full-scale clicks for the rest of the session. A later rate change has cleared it. Details in `.claude/re/open_questions.md`.
- **MIDI**: OzzyMIDI is installed under `/Library/Audio/MIDI Drivers` and reads MIDI from the same shared memory as the HAL; it needs no SIP change either.

### Security

Accepted risks of the shared memory model:

- The segment `/ozzy_shared_mem` is created with mode 0666. The daemon runs as the logged-in user while `coreaudiod` runs as `_coreaudiod`, and a non-root process cannot chown the segment, so any local user can read or write the audio buffers.
- Another local user can create the segment first (squatting the name) and so stop the driver from starting. OzzyHAL refuses to map a segment that is not owned by the console user, so this is denial of service, not injection.

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
