Userspace macOS driver for the Dynacord CMS 600-3: 4 inputs and 4 outputs at 44.1, 48 and 96 kHz, plus both MIDI ports (USB-MIDI and SystemCtrl). Needs an Apple Silicon Mac on macOS 26.1 or later, and no SIP changes.

## What's new in 0.5.0

This release is rebuilt on upstream Ozzy's own userspace daemon, which upstream now ships as install option 3. The fork keeps its CMS 600-3 extras on top: the SystemCtrl MIDI port, named channels, volume and mute, the MON and AUX devices, and devices that ride out a driver restart.

### What changes

- The daemon now runs once, as root, from boot (`/usr/local/bin/OzzyDaemon`, started by `/Library/LaunchDaemons/com.ozzy.daemon.plist`), instead of once per logged-in user. The mixer no longer moves between users' daemons on a user switch.
- Its log is in the system log (subsystem `OzzyDaemon`), not `~/Library/Logs/OzzyDaemon.log`. `macos/debuglogs.command` collects it with the plug-ins' logs.
- 12 ms of output is queued, not 24 ms, so playback latency is lower. It ran without a dropout on the mixer at 44.1, 48 and 96 kHz.
- The driver starts the mixer at 96 kHz and then switches to the rate set in Audio MIDI Setup.
- If the mixer stops streaming, the driver restarts it, as before. A restart that fails now leaves it stopped until you replug it, where 0.4.x kept retrying.
- Upstream fixes come with it, among them better handling of short input transfers and of the mixer's feedback at start.

### Upgrading

- From 0.4.x: the installer stops and removes 0.4.x's per-user daemon before it starts the new one. Settings carry over.
- From 0.3.x: the output volume and mute set with 0.3.x carry over. The mixer's device ID includes its USB port, as it has no serial number, so after an upgrade from 0.3.x it appears as a new device once and apps may need it picked again; in another USB port it is another device with its own settings.
- If `/usr/local/bin` on your Mac can be written by users other than root (an old Homebrew install can leave it so), the installer stops and says how to fix it: the daemon runs from there as root.

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
