Userspace macOS driver for the Dynacord CMS 600-3: 4 inputs and 4 outputs at 44.1, 48 and 96 kHz, plus both MIDI ports (USB-MIDI and SystemCtrl). Needs an Apple Silicon Mac on macOS 26.1 or later, and no SIP changes.

## What's new in 0.4.2

### New

- An input volume and mute for the CMS 600-3, next to the output ones: Sound settings and Control Center show an input level slider and mute, and apps can set them like any input device's. They use the output's curve (-60 to 0 dB) and are kept per device.

### Reliability

- If the driver process restarts (a crash, or its LaunchAgent being restarted), apps playing or recording keep the device: it stays listed and silent for up to 3 s, and carries on at the same sample rate and settings once the driver is back. 0.4.1 removed it, and apps had to pick it again. MON and AUX recordings carry on too.
- A driver that can't stop the mixer's USB transfers when it shuts the mixer down keeps the memory they may still write, instead of freeing it under them.
- `~/Library/Logs/OzzyDaemon.log` is rotated at 5 MB (the previous one is kept as `OzzyDaemon.log.1`), and a log file that can't be opened is reported in the system log instead of failing silently. `debuglogs.command` collects both.
- Lost USB feedback packets (a macOS scheduling hiccup) no longer count towards the limit that switches feedback pacing off for the rest of the session, as in 0.3.0.
- The audio plug-in checks the sample rate, safety offsets and timestamp spacing the driver publishes before using them.
- The MIDI plug-in finds a device's existing port again even when the device's name fills its whole name field.

### Linux

- A USB transfer the kernel refuses to resubmit stops the streams with an XRUN (the next prepare restarts the mixer) instead of silently leaving fewer transfers queued.
- Feedback only counts as alive when its value is in range, so the 50 ms stall check is not fooled by packets of zeros from a halted mixer.
- Checked on the mixer for the first time since 0.4.0: playback and capture at 44.1, 48 and 96 kHz, both MIDI ports, an underrun giving an XRUN, and a failed restart leaving the mixer's USB alone.

### Uninstalling the kext

- `uninstall.command` now says when the kext stays loaded until a reboot, and rebuilds or explains the auxiliary kernel collection, so an old kext can't come back at boot and take the mixer from the userspace driver. The driver conflict message names it correctly.

### Upgrading

- From 0.4.x: nothing to do; settings carry over.
- From 0.3.x: the output volume and mute set with 0.3.x carry over. The mixer's device ID includes its USB port, as it has no serial number, so after an upgrade from 0.3.x it appears as a new device once and apps may need it picked again; in another USB port it is another device with its own settings. The first time the driver runs it comes up at 48 kHz; after that it keeps the rate you set.

## Install with Homebrew

```bash
brew install --cask dominics/tap/ozzy-cms600
```

## Or install the package

Download the `.pkg` below. It is not signed with an Apple Developer ID, so macOS blocks it the first time you open it:

1. Double-click the package, then close the warning.
2. Open System Settings > Privacy & Security, scroll to Security, and click **Open Anyway** next to the message about the package.
3. Confirm the warnings that follow, and the installer opens.

Or skip Gatekeeper from Terminal: `sudo installer -pkg Ozzy-CMS600-*.pkg -target /`

## Uninstall

`brew uninstall --cask ozzy-cms600`, or `sudo "/Library/Application Support/Ozzy/uninstall.sh"`.

More in the [README](https://github.com/dominics/Ozzy/blob/release/README.md#about-this-fork).
