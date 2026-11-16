# Userspace driver package (Dynacord CMS 600-3, SIP enabled)

The fork's package for the Dynacord CMS 600-3 needs no kernel extension and no change to System Integrity Protection. It installs upstream's userspace daemon, `OzzyDaemon` (install option 3 in upstream's [README](../README.md)), with the fork's OzzyHAL (Core Audio) and OzzyMIDI (CoreMIDI) plug-ins. The daemon talks to the mixer through macOS's userspace USB API (IOUSBLib) and shares each device's audio and MIDI buffers with the plug-ins through a POSIX shared memory object, `/ozzy.<USB locationID as 8 hex digits>`, listed in its device table `/ozzy.devices`.

Install and uninstall are described in the [README](../README.md#about-this-fork). The package (`com.ozzy.userspace`) installs `/usr/local/bin/OzzyDaemon` and `/Library/LaunchDaemons/com.ozzy.daemon.plist`, which runs it as root from boot, at the same paths as install option 3.

## Notes

- **Supported Macs**: Apple silicon (arm64) on macOS 26.1 or later; the package refuses anything else. It refuses to install over upstream's kext driver too, as both claim the mixer.
- **Upgrading from 0.4.x**: 0.4.x ran its own daemon as a LaunchAgent in each GUI session. The package stops those and removes `/Library/LaunchAgents/com.ozzy.daemon.plist` and `/Library/Application Support/Ozzy/OzzyDaemon` before it starts the LaunchDaemon. Device settings (volume, mute, channel names) are kept.
- **Power-cycle quirk**: the mixer only enumerates if it is powered on with the USB cable already plugged in. If it does not show up, power it off, plug in the cable, then power it on.
- **Sample rate**: 44.1, 48 or 96 kHz, set in Audio MIDI Setup. The daemon starts a device at its highest rate (96 kHz); the HAL then gives it the rate the device had in Core Audio.
- **One device per USB port**: the mixer has no serial number, so its device UID carries the USB port. Plugged into another port it shows up as a new device, with its own volume, mute and channel settings.
- **MIDI**: two ports, "USB-MIDI" (the DIN sockets) and "SystemCtrl" (FX control).
- **MON and AUX**: the mixer's USB inputs 4 and 3 also show up as two mono devices, "CMS 600-3 MON" and "CMS 600-3 AUX", related to the main device so an aggregate of them needs no resampling.
- **A device that stops streaming**: the engine's watchdog restarts it when a PCM pipe has had no completion for 500 ms, at most once each 10 s. A restart that fails leaves the device stopped until it is replugged. A restart under running IO (also on wake) restarts Core Audio's IO on the new clock.
- **A daemon restart or a quick replug**: when the daemon goes away (`sudo launchctl kickstart -k system/com.ozzy.daemon`, a crash) or the mixer is unplugged, the main, MON and AUX devices stay in Core Audio for 3 s, silent and on a clock that keeps running. If the daemon has the mixer again on the same USB port within that time, apps using the devices carry on: the rate and device settings the devices had are given back to the daemon, which starts with its defaults, through a short format change. Otherwise the devices are withdrawn, and an app holding one has to pick the device again.
- **Sleep**: the daemon suspends the stream when the Mac sleeps and starts it again on wake.
- **Logs**: the daemon and the plug-ins log to the system log, subsystems `OzzyDaemon`, `OzzyHAL` and `OzzyMIDI`: `log stream --predicate 'subsystem == "OzzyDaemon" OR subsystem == "OzzyHAL" OR subsystem == "OzzyMIDI"'`. `macos/debuglogs.command` collects them.

## Security

The daemon runs as root. It creates each device's shared memory with mode 0666, so that `coreaudiod` and `MIDIServer` can map it, which means any local user can read or write the audio buffers and MIDI rings. The engine masks the MIDI ring indices before using them, so a bad index written there can't make the root daemon write outside its rings.
