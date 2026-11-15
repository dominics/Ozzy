# TODO

Open work on this fork's Dynacord CMS 600-3 support (the macOS userspace driver, the MIDI second port and the CMS rows of the shared engine and Linux module), as of 0.4.2. Upstream's own devices and its kext are out of scope here.

## Hardware checks before tagging a release

Run with the mixer's loopback (outputs 1-2 and 3-4 back into inputs 1-2), recording the USB input rather than listening.

- [x] 44.1, 48 and 96 kHz clean through the loopback (30 s each, 300 s at 48 kHz), with both MIDI ports looping during the audio.
- [ ] Start rate: the first attach comes up at 48 kHz; a replug or a user switch back keeps the rate last chosen.
- [ ] A rate change while another app records from MON and AUX: no glitch beyond the switch, and their formats follow.
- [ ] The 24 ms OUT queue: no recoveries of pipe 0x02 under load (load above the core count for 120 s). 0.4.0-rc.1 had one "no completion" recovery of 0x02 at a load of about 20, not seen again at a load of 3.
- [x] A late PCM OUT submit: a daemon frozen 30 ms or more makes PCM OUT late and halts the firmware each time (20 ms only delays feedback), so the recovery on a late submit stays.
- [ ] After the feedback latch fix (0.4.2): freeze the daemon 100 to 300 ms during a 48 kHz loopback (`latetest.sh` / `freeze20.sh`) and expect no "more than 40 feedback packets" line.
- [x] Kill coreaudiod during playback: the mixer goes silent rather than looping the last buffer.
- [x] Power-cycle the mixer with USB connected: it comes back through a fresh connect and streams about 1.5 s after power-on.
- [ ] A failed start is retried (4 x 500 ms, then each 2 s): unplug and replug during the handshake.
- [ ] No false watchdog recoveries in 10 minutes idle and 10 minutes streaming, including bulk 0x86 while no app has the device open.
- [ ] Upgrades from 0.4.0 and from 0.3.0 keep a lowered output volume and mute.
- [ ] The input volume and mute in Sound settings lower and silence the loopback recording without clicks, and both are kept across a replug and a `coreaudiod` restart.
- [x] Linux: a forced underrun stops the stream with an XRUN, and a failed rate change leaves the mixer stopped rather than resetting its USB.

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

- [ ] An app that held the device across a replug or a daemon restart never resumed, because the HAL withdrew the device and coreaudiod published it again under a new AudioObjectID (0.3.0's HAL withdrew it too). Since 0.4.2 the HAL keeps a device that lost its driver published for 3 s (`kOzzyGraceNs`), silent and on a clock moved on at the nominal rate, and maps a driver that comes back with the same UID into the same slot; a different rate or device settings go back to the driver through a configuration change. A daemon restart (`launchctl kickstart -k gui/$UID/com.ozzy.daemon`) with an app playing to the main device and another recording MON and AUX passes at 48 and 96 kHz: both carry on after a short gap. Still to check on hardware, with the same two apps: a replug on the same port inside 3 s (kept), a replug after more than 3 s (withdrawn, as before), and a user switch while playing; watch for the HAL's `lost its driver`, `its driver is back` and `withdrawing it` lines.
- [x] Periodic digital distortion (a crunch every 1-2 s, on all apps) after the daemon recovered the device under running IO, until IO restarted (a rate change cleared it). The engine's clock starts over at a recovery, but CoreAudio kept its IO on the old timeline despite the new seed, so the HAL wrote output millions of frames away from where the engine read and the two slid past each other in the ring. The HAL now asks for a configuration change when the engine restarts on its own, or its driver comes back within the grace, with IO running and the same layout, so IO restarts on the new clock. Checked on hardware (2026-11-16): an engine restart through `OZZY_METHOD_APPLY_SETTING`, a watchdog recovery (daemon stopped for 0.7 s) and a daemon `SIGKILL` all keep the HAL's write position about one IO buffer plus the safety offset ahead of the clock.
- [ ] Watchdog recoveries on one test Mac are frequent at low load: 34 in the log from 2 to 16 Nov, 4 in the first two hours on 16 Nov (`pipe 0x02: no completion` and `pipe 0x81: no feedback`, load about 3). Find what stalls the pipes; each recovery is a ~350 ms dropout even with the HAL re-sync.
- [ ] Clicks on inputs 1 and 3 after a rate change: in some 48 and 96 kHz sessions (never 44.1 kHz) byte 0 of the 64-byte input frame, which holds bit 23 of inputs 1 and 3, repeats the previous frame's, so quiet signals there click at full scale until the next rate change. Seen on macOS and, far more often, on Linux. A usbmon capture (2026-11-04) shows the mixer sends the stale byte itself; restoring 0.3.0's five alternating SET_CUR does not help. Accepted for now, no fix planned; a host-side repair (copy bit 22 into bit 23 when only that frame disagrees) or a re-roll on detection would be the options. Notes in [.claude/re/open_questions.md](.claude/re/open_questions.md).
- [ ] Check whether OzzyMIDI still drops MIDI bytes after a MIDIServer restart until the device-online block has run, and whether the HAL still sends a duplicate `PropertiesChanged(NominalSampleRate)` (both seen with 0.3.x).
- [ ] Linux on the CMS 600-3: short loopbacks at 44.1, 48 and 96 kHz and both MIDI ports pass; still to run are 5-minute runs at each rate and an unplug while streaming. Also from the port: the minimum playback period is now 768 frames (JACK `-p 256` and `-p 512` are refused) and the rawmidi port names are cut at 32 characters.
- [ ] With more than 2 transfers in flight on an IN pipe, the daemon's re-anchoring of a late transfer ("late, its queue ran dry ... resumed at frame N", `macos/Backends/OzzyDaemon/user_bus.c`) moves only the resubmitted transfer to a new frame, and the stream seems to stay out of order until a restart. 0.4.2 keeps 2 PCM IN reads, but fix this before deepening any IN queue (deeper queues damaged recordings after a 20 ms stall; kept on `archive/pcm-in-4-reads`).
- [x] An idle MON or AUX device can read silence until it is unmapped: `mMonoChanging` is cleared only by that device's Perform or Abort, a failed request or Restore, not by StartIO (`macos/OzzyHAL/OzzyHALMono.h`), so a rate request CoreAudio accepts but never performs leaves it set. The first StartIO now clears it, unless the main device's change is still running.
- [x] `macos/install.command` option 3 ("Userspace Daemon") still says NOT YET IMPLEMENTED, although `install-userspace.command` and the .pkg exist. It now runs `install-userspace.command`.
- [ ] Next release notes: the CoreAudio IO buffer defaults to at most 1200 frames at all rates (0.3.0 allowed about 3600 at 96 kHz); raise it in the settings for larger buffers.
- [x] CI ran only on pushes to `release` and on `v*` tags, so a regression on the trunk showed up at release time. Pushes to `dynacord-cms600` now run the lint and host tests too, without building the package.

## Features asked for

- [x] Input volume and mute, like the output ones, on the main device's input (`macos/OzzyHAL/OzzyHALExtras.h`).

## Test tools

- [x] The hardware test tools used for releases (a duplex loopback recorder with a two-tone analyser, and a MIDI port loop / SysEx echo by port name) are in `tools/cms600`, with `loopback.sh` for the release run.
