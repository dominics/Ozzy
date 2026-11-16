# TODO

Open work on this fork's Dynacord CMS 600-3 support (the macOS package of upstream's userspace daemon, the fork's HAL and MIDI extras, the second MIDI port in the shared engine and Linux module), as of the rebuild on upstream's daemon after 0.4.3. Upstream's own devices and its kext are out of scope here.

## Hardware checks before tagging a release

Run with the mixer's loopback (outputs 1-2 and 3-4 back into inputs 1-2), recording the USB input rather than listening (`tools/cms600`).

- [x] Upstream's daemon on the CMS 600-3 (checked on the mixer, 2026-11-16, with upstream's own HAL): 44.1, 48 and 96 kHz clean through the loopback (0 overloads, 0 late or short transfers, 0 watchdog recoveries, also over a minute idle), the first MIDI port looping and echoing SysEx, MON and AUX carrying a voice, a replug reattaching within 7 s.
- [ ] The package over 0.4.3: the per-user LaunchAgent is gone, exactly one `OzzyDaemon` runs (as root), and the device shows up without a second `coreaudiod` restart. With upstream's install by hand it took a second restart; the postinstall now waits for the daemon's handshake first.
- [ ] A daemon restart while an app records (`sudo launchctl kickstart -k system/com.ozzy.daemon`): the recording carries on after a short gap, 0 glitches after it, and the level the same as before. With upstream's own HAL the recorder froze, and the first recording after the restart came back 6 dB louder with one glitch at 5 s.
- [ ] Both MIDI ports (USB-MIDI and SystemCtrl) on the package, the MON and AUX devices, and output and input volume and mute.
- [ ] A replug on the same port inside 3 s (the devices are kept) and after more than 3 s (withdrawn).
- [ ] A rate change while another app records from MON and AUX: no glitch beyond the switch, and their formats follow.
- [ ] Upgrades from 0.4.3 and from 0.3.0 keep a lowered output volume and mute.
- [ ] Linux: the module with the second MIDI port on the shared memory's rings, both rawmidi substreams looping.

## Watch on hardware: fork behaviour dropped for upstream's

The rebuild took upstream's behaviour where the fork's rested on 0.3.0-era reasoning. Bring one back only if it shows up on hardware.

- [ ] Bulk PCM IN (0x86) going quiet after a switch to 96 kHz or while idle, which 0.4.0-rc.1 read as a dead pipe and recovered. Upstream's watchdog watches it; no false recovery in the 2026-11-16 run. A real halt also silences 0x86, so the watchdog catches that too.
- [ ] Output queue: 12 ms (3 transfers) as upstream, not the fork's 24 ms. Watch the watchdog and `ozzy-cli show`'s late counts under load (load above the core count for 120 s).
- [ ] A start or restart that fails leaves the device stopped until a replug (the fork retried each 2 s). If it happens, propose a bounded retry in upstream's daemon.
- [ ] Linux USB resets the mixer after a failed restart or watchdog recovery (the fork never reset the CMS 600-3, on a single pcap-era note). Test once with a forced failed restart; re-add the quirk only if the mixer then fails to enumerate.
- [ ] Start rate: the daemon starts at 96 kHz and the HAL switches it to the app's rate (two handshakes on each attach). Propose a 48 kHz or last-set start upstream if the extra handshake costs anything.

## To offer upstream

- [ ] The second MIDI port: `midi2_out` / `midi2_in` at the end of `struct ozzy_shared_memory` (version 14), the engine side, Linux's second substream and OzzyMIDI's second entity.
- [ ] The HAL's IO restart when the engine restarts under it (`info.starts`, the (0, 0) anchor, the seed): upstream's watchdog makes it reachable on any device.
- [ ] Engine fixes: the anchor reset through the seqlock, a ring attached again at the same address, masked MIDI ring indices (the shared memory is 0666 and the daemon root), the input ring cleared at start, sent output frames silenced, lost feedback packets not counted towards the 40 limit.
- [ ] `uninstall.command` reporting a kext that stays loaded, and its kernel collection.
- [ ] The CMS 600-3 pacing test against the Windows driver's captures (`tests/test_isoc_golden.c`).
- [ ] The daemon's shared memory and mach service (a security review of upstream's daemon): OzzyHAL and OzzyMIDI map `/ozzy.devices` and `/ozzy.<location>` without checking that root created them (another local user could create them first; 0.4.x checked the owner), the per-device objects are 0666 though root writes them, and the mach service takes calls from anyone with the session ID from the world-readable table. The fork's clients now refuse objects not owned by root or too small (and a table others can write): offer that, then a 0660 `_coreaudiod` group, and the caller's audit token on calls.
- [ ] Hardening found by reading, dropped from the fork: an arm64 ordering of the completion count against the ring detach (Linux), memory IOKit may still write after a stuck abort (daemon), the MIDI entity's `MIDIDeviceAddEntity` result (OzzyMIDI).

## Open issues

- [ ] Clicks on inputs 1 and 3 after a rate change: in some 48 and 96 kHz sessions (never 44.1 kHz) byte 0 of the 64-byte input frame, which holds bit 23 of inputs 1 and 3, repeats the previous frame's, so quiet signals there click at full scale until the next rate change. Seen on macOS and, far more often, on Linux. A usbmon capture (2026-11-04) shows the mixer sends the stale byte itself; restoring 0.3.0's five alternating SET_CUR does not help. Accepted for now, no fix planned; a host-side repair (copy bit 22 into bit 23 when only that frame disagrees) or a re-roll on detection would be the options. Notes in [.claude/re/open_questions.md](.claude/re/open_questions.md).
- [ ] Check whether OzzyMIDI still drops MIDI bytes after a MIDIServer restart until the device-online block has run, and whether the HAL still sends a duplicate `PropertiesChanged(NominalSampleRate)` (both seen with 0.3.x).
- [ ] Linux on the CMS 600-3: 5-minute runs at each rate and an unplug while streaming. Also from the port: the minimum playback period is 768 frames (JACK `-p 256` and `-p 512` are refused) and the rawmidi port names are cut at 32 characters.
- [ ] Next release notes: the CoreAudio IO buffer defaults to at most 1200 frames at all rates (0.3.0 allowed about 3600 at 96 kHz); raise it in the settings for larger buffers.

## Values to measure on hardware

- [ ] Safety offsets with the 12 ms queue against the loopback round trip the timestamps don't cover; the device reports a latency of 0.
- [ ] The rate read-back after SET_CUR uses wIndex 0; the Windows driver sends 0x86.
