Userspace macOS driver for the Dynacord CMS 600-3: 4 inputs and 4 outputs at 44.1, 48 and 96 kHz, plus both MIDI ports (USB-MIDI and SystemCtrl). Needs an Apple Silicon Mac on macOS 26.1 or later, and no SIP changes.

## What's new in 0.4.3

### Reliability

- No more periodic digital distortion (a crunch every second or two, in all apps) after the driver recovered the mixer while audio was running. The driver's clock started over, but apps kept going on the old timeline until something restarted their audio, such as a sample rate change. Now the audio restarts on the new clock as soon as the driver recovers, after a USB stall, a wake from sleep or a driver restart.
- A MON or AUX input device no longer stays silent when macOS accepted a change to it while it was idle and never carried it out. Recording from it again brings its input back.

### Installing from source

- Option 3 of `install.command` (the userspace driver) now builds and installs the package instead of saying it isn't implemented yet.
- `install.command` recognises an existing userspace install, and no longer mistakes it for the kext and asks you to uninstall it.

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
