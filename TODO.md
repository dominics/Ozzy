# TODO

Open work on this fork's Dynacord CMS 600-3 support (the macOS userspace driver, the MIDI second port and the CMS rows of the shared engine and Linux module), as of 0.4.1. Upstream's own devices and its kext are out of scope here.

## Hardware checks before tagging a release

Run with the mixer's loopback (outputs 1-2 and 3-4 back into inputs 1-2), recording the USB input rather than listening.

- [ ] 44.1, 48 and 96 kHz clean through the loopback (30 s each, 300 s at 48 kHz), with both MIDI ports looping during the audio.
- [ ] Start rate: the first attach comes up at 48 kHz; a replug or a user switch back keeps the rate last chosen.
- [ ] A rate change while another app records from MON and AUX: no glitch beyond the switch, and their formats follow.
- [ ] The 24 ms OUT queue: no recoveries of pipe 0x02 under load (load above the core count for 120 s). 0.4.0-rc.1 had one "no completion" recovery of 0x02 at a load of about 20, not seen again at a load of 3.
- [ ] A late PCM OUT submit (OzzyDaemon now recovers the device on it): provoke one under load. If feedback stays live after the late transfer, drop the recovery trigger and keep the log line; if feedback goes empty, the firmware halted and the recovery stays.
- [ ] Kill coreaudiod during playback: the mixer goes silent rather than looping the last buffer.
- [ ] A failed start is retried: unplug and replug during the handshake, and power-cycle the mixer with USB connected.
- [ ] No false watchdog recoveries in 10 minutes idle and 10 minutes streaming, including bulk 0x86 while no app has the device open.
- [ ] Upgrades from 0.4.0 and from 0.3.0 keep a lowered output volume and mute.
- [ ] The input volume and mute in Sound settings lower and silence the loopback recording without clicks, and both are kept across a replug and a `coreaudiod` restart.
- [ ] Linux: a forced underrun stops the stream with an XRUN, and a failed rate change leaves the mixer stopped rather than resetting its USB.

## Values to measure on hardware

These were set without a measurement behind them.

- [ ] The first isochronous transfer starts `ISOC_LEAD_FRAMES` (10) frames out; 0.3.0 used 2 with nothing recorded about why (`macos/Backends/OzzyDaemon/user_bus.c`).
- [ ] The watchdog's error streak of 3 failed completions (`ERROR_STREAK`) and its 1 s between recoveries (`WATCH_RECOVER_MS`), in `macos/Backends/OzzyDaemon/device.c`.
- [ ] Safety offsets: output 1344 frames at 48 kHz (the 24 ms queue; 0.3.0 used 40 ms), input 160 frames (0.3.0 used 25 ms). Measure the output lead and the loopback round trip the timestamps don't cover; the device reports a latency of 0.
- [ ] The rate read-back after SET_CUR uses wIndex 0; the Windows driver sends 0x86.
- [ ] MIDI IN reads 9 bytes per transfer (0.3.0 read 512): check the lengths of 0x83 packets in the Windows captures.
- [ ] `GetBusMicroFrameNumber` is frame x 8 + microframe, so the isochronous start frames and microframe counts line up: log both for the first seconds of a run.
- [ ] Count transfers that wrap the ring against the glitch telemetry, and check the per-packet times against the feedback rate.

## Open issues

- [ ] An app that holds the device across a replug or a daemon restart never resumes, because the HAL withdrew the device and coreaudiod publishes it again under a new AudioObjectID (0.3.0's HAL withdrew it too). The HAL now keeps a device that lost its driver published for 3 s (`kOzzyGraceNs`), silent and on a clock moved on at the nominal rate, and maps a driver that comes back with the same UID into the same slot; a different rate or device settings go back to the driver through a configuration change. To check on hardware: `launchctl kickstart -k gui/$UID/com.ozzy.daemon` with an app playing to the main device and another recording MON and AUX (both carry on, after a short gap), the same with a replug on the same port inside 3 s, and a replug after more than 3 s (withdrawn, as before).
- [ ] A possible stale byte 0 on bulk PCM IN: in some 48 kHz sessions started right after a rate change (0.3.x, OzzyDaemon over IOUSBLib), byte 0 of the 64-byte input frame repeated the previous frame's in about 8% of frames. It holds bit 23 of inputs 1 and 3, so quiet signals there click at full scale. Not reproduced since, and a later rate change cleared it; suspect it first if inputs 1 or 3 click. Notes in [.claude/re/open_questions.md](.claude/re/open_questions.md).
- [ ] Check whether OzzyMIDI still drops MIDI bytes after a MIDIServer restart until the device-online block has run, and whether the HAL still sends a duplicate `PropertiesChanged(NominalSampleRate)` (both seen with 0.3.x).
- [ ] Linux has only been compiled for 0.4.x, never run on the CMS 600-3: 5-minute runs at each rate, both MIDI ports, unplug while streaming. Also from the port: the minimum playback period is now 768 frames (JACK `-p 256` and `-p 512` are refused) and the rawmidi port names are cut at 32 characters.
- [x] CI ran only on pushes to `release` and on `v*` tags, so a regression on the trunk showed up at release time. Pushes to `dynacord-cms600` now run the lint and host tests too, without building the package.

## Features asked for

- [x] Input volume and mute, like the output ones, on the main device's input (`macos/OzzyHAL/OzzyHALExtras.h`).

## Test tools

- [x] The hardware test tools used for releases (a duplex loopback recorder with a two-tone analyser, and a MIDI port loop / SysEx echo by port name) are in `tools/cms600`, with `loopback.sh` for the release run.
