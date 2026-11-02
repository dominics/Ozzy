Userspace macOS driver for the Dynacord CMS 600-3: 4 inputs and 4 outputs at 44.1, 48 and 96 kHz, plus both MIDI ports (USB-MIDI and SystemCtrl). Needs an Apple Silicon Mac on macOS 26.1 or later, and no SIP changes.

## What's new in 0.4.1

0.4.0 moved the driver onto upstream Ozzy's rewrite. 0.4.1 brings back behaviour from 0.3.0 that the move lost, and fixes problems found reviewing it.

### Reliability

- A start that fails when the mixer is plugged in (a USB request timing out, say) is tried again: 5 tries half a second apart, then each 2 s while the mixer stays plugged in. 0.4.0 waited for a replug.
- A restart that fails after a sample rate change, a setting change or a wake from sleep now releases the mixer and tries again each 2 s, instead of leaving it silent until a replug.
- A mixer that stops streaming is noticed and restarted sooner: 3 failed transfers in a row on any USB pipe, a transfer the system refused, or an output queue that ran dry (which halts the mixer's firmware). Restarts can come 1 s apart instead of 10 s.
- The driver starts the mixer at 48 kHz, or at the rate last set on that USB port, instead of at 96 kHz each time. An app running at 48 kHz no longer sees a switch to 96 kHz and back after a replug, a user switch or a recovery.
- The driver's heartbeat keeps going while it sets up the mixer, so a slow start isn't taken for a driver that has gone away.
- A sample rate change the driver finishes after Core Audio stopped waiting for it is now picked up.

### Audio

- The output queue is 24 ms again (0.4.0: 12 ms), so a few ms of macOS scheduling delay no longer let it run dry and halt the mixer.
- If `coreaudiod` quits during playback, the mixer goes silent instead of repeating the last fraction of a second.
- A short or failed USB input read gives silence instead of old audio, and the input starts from silence after a restart.
- An app sending NaN samples is sent silence, not full-scale noise.
- MON and AUX follow a sample rate change of the main device again (reading silence while it happens), run on clocks of their own so starting or stopping the main device leaves them alone, and are named "CMS 600-3 MON" and "CMS 600-3 AUX" again. All three are related devices again, so an aggregate of them needs no resampling.

### Security

- The audio and MIDI plug-ins again use the driver's shared memory only if the user in front owns it, so another local user can't feed them audio or MIDI.
- Values read from the shared memory (MIDI ring positions, the ring layout, device names) are checked before use, so a process writing it can't make the driver or the plug-ins read or write out of bounds.

### Linux

These change the Linux module's support for the CMS 600-3, which has been compiled but not yet run on the mixer since 0.4.0.

- A restart that fails no longer resets the mixer's USB, after which the mixer may only come back when power-cycled. The stream stays stopped and the next open or prepare tries again.
- When the mixer stops sending feedback for 50 ms (its firmware halted), the streams stop with an XRUN instead of playing on into a silent device; the next prepare restarts the mixer.
- The output queue is 24 ms, as on macOS.

### Upgrading

- From 0.3.x: the output volume and mute set with 0.3.x now carry over (0.4.0 came up at full volume, unmuted), unless the mixer already has any setting of its own from 0.4.0 (volume, mute or stereo pair), which then wins. Since 0.4.0 the mixer's device ID includes its USB port, as it has no serial number: after an upgrade from 0.3.x it appears as a new device once, so apps may need it picked again, and in another USB port it is another device with its own settings.
- The first time 0.4.1 drives the mixer it comes up at 48 kHz; after that it keeps the rate you set.
- `debuglogs.command` now includes the userspace driver's own log, `~/Library/Logs/OzzyDaemon.log`, and whether its LaunchAgent is running.

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
