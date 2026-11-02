# Userspace driver (Dynacord CMS 600-3, SIP enabled)

The fork's macOS driver for the Dynacord CMS 600-3 needs no kernel extension and no change to System Integrity Protection. Upstream's OzzyHAL (Core Audio) and OzzyMIDI (CoreMIDI) plug-ins are fed by `OzzyDaemon`, a program that talks to the mixer through macOS's userspace USB API (IOUSBLib) and shares the audio and MIDI buffers with the plug-ins through a POSIX shared memory segment per device, `/ozzy.<USB locationID as 8 hex digits>`.

Install and uninstall are described in the [README](../README.md#about-this-fork). The package (`com.ozzy.userspace`) installs `/Library/LaunchAgents/com.ozzy.daemon.plist`, which starts `/Library/Application Support/Ozzy/OzzyDaemon --log-file` in each GUI session.

## Notes

- **Supported Macs**: Apple silicon (arm64) on macOS 26.1 or later; the package refuses anything else. It refuses to install over upstream's kext driver too, as both claim the mixer.
- **CMS 600-3 only**: the package drives the CMS 600-3 and nothing else. The Allen & Heath Xone devices and the rest of upstream's list use upstream's kext driver.
- **Power-cycle quirk**: the mixer only enumerates if it is powered on with the USB cable already plugged in. If it does not show up, power it off, plug in the cable, then power it on.
- **Sample rate**: 44.1, 48 or 96 kHz, set in Audio MIDI Setup. The first time a daemon drives the mixer it starts at 48 kHz; after that it starts at the rate last set on that USB port, so a replug or a recovery does not switch a running app's rate. Each user's daemon keeps its own, until it exits at logout.
- **One device per USB port**: the mixer has no serial number, so its device UID carries the USB port. Plugged into another port it shows up as a new device, with its own volume, mute and channel settings.
- **MIDI**: two ports, "USB-MIDI" (the DIN sockets) and "SystemCtrl" (FX control).
- **MON and AUX**: the mixer's USB inputs 3 and 4 also show up as two mono devices, "CMS 600-3 MON" and "CMS 600-3 AUX", related to the main device so an aggregate of them needs no resampling.
- **Several logged-in users**: each GUI session runs its own daemon, and only the console user's (the one in front) drives the mixer. On a user switch the old daemon releases the mixer and its segment and the new one takes over, retrying each 100 ms for up to 5 s while the old one lets go, then each 2 s for as long as its user is in front and the mixer is plugged in. Anything else that holds the mixer (`ploytec-play`, the kext) makes it wait the same way, and while the daemon streams it holds the device, so other tools cannot open it.
- **A start that fails** (a control request timing out at plug-in, say) is tried again 4 times, 500 ms apart, then each 2 s while the mixer stays plugged in. A device that stops streaming (no feedback for 1 s, or 3 failed transfers in a row on one pipe) is restarted; if that fails too, the daemon releases it and tries again each 2 s.
- **Sleep**: the daemon holds no sleep assertion. When the Mac sleeps it suspends the stream, and on wake it runs the start sequence again. For long recordings on battery use `caffeinate -i` or a power assertion from the recording app.
- **Latency**: output is queued as 6 x 4 ms isochronous transfers (24 ms), and the output safety offset Core Audio is told covers that queue: 1344 frames at 44.1 and 48 kHz, 2496 at 96 kHz. The input safety offset is much smaller. These are not yet measured on hardware against the mixer's loopback; see [TODO.md](../TODO.md).
- **Logs**: the daemon writes `~/Library/Logs/OzzyDaemon.log` only (not the system log). The plug-ins log to the system log: `log stream --predicate 'subsystem == "OzzyHAL" OR subsystem == "OzzyMIDI"'`. `debuglogs.command` collects both.

## Security

The LaunchAgent runs for each user who logs in, so `OzzyDaemon` lives in `/Library/Application Support/Ozzy`, which only root can write.

Accepted risks of the shared memory model:

- The segment is created with mode 0666. The daemon runs as the logged-in user while `coreaudiod` runs as `_coreaudiod`, and a non-root process cannot chown a POSIX shared memory segment, so any local user can read or write the audio buffers and MIDI rings.
- Another local user can create a device's segment first (squatting the name) and so stop the driver from starting. OzzyHAL and OzzyMIDI map a segment only if the console user owns it, so this is denial of service, not injection.
- What the segment holds is not trusted: the daemon and the plug-ins mask the MIDI ring indices before using them, and the plug-ins bound the ring layout and copy the device names with a fixed size.
