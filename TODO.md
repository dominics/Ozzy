# TODO

Open work on the Dynacord CMS 600-3 support, last checked on 2026-10-26 against `dynacord-cms600`. The macOS userspace driver (v0.2.0, Homebrew cask `ozzy-cms600`) is in daily use without faults. Reverse-engineering TODOs stay in `~/workspace/dynacord-cms-pcap/TODO.md`.

## Linux ALSA driver (`linux/devices/dynacord.c`)

Playback, capture, two-port MIDI and unplug/replug work on spork (kernel 6.8).

- [x] Security review triage, done on 2026-10-26 by reading the code; the fixes are compile-tested on 7.0, not yet on hardware:
  - [x] Real: disconnect or `pre_reset` running `ozzy_pcm_abort` (no `stream_mutex`) during a prepare whose `set_rate` stops and restarts the device's URBs. That gave two concurrent `stop_urbs` freeing the same URBs, or a restart after the abort. Fixed in "fix(linux): serialise ozzy_pcm_abort with prepare".
  - [x] Real, theoretical window: `snd_pcm_period_elapsed()` after dropping `sub->lock`, while the URBs keep running after a trigger stop, so hw_free or close could free the runtime under it. The core handlers also read `sub->instance` after the unlock. Fixed with a `sync_stop` callback that waits for in-flight reports, in "fix(linux): wait for in-flight period reports before ALSA frees a runtime".
  - [x] Not a use-after-free: `dynacord_init` dropping `priv` after a failed post-reset handshake. URBs are already stopped by `pre_reset`, every op checks for a NULL `priv`, and the rawmidi device owns `priv` once created. The `midi_out` completion can't outlive `stop_urbs`, which unpublishes it under `midi_lock` and then kills it.
  - [x] Not exploitable: device input. Capture is bounded by `actual_length` (capped by the USB core) and a ring of at least 6912 bytes, against at most 3456 written per URB. Feedback offsets are set by the driver. `dynacord_midi_in_slot` checks the packet length and stays within its slot.
- [ ] Hardware-test the two fixes on spork: playback / capture / rate changes, and unplug while changing rate (`speaker-test` with `-r` flips).
- [ ] A post-reset re-init after a failed one allocates a new `priv` and creates a second rawmidi device, so MIDI breaks until replug. Functional, not memory safety.
- [ ] Recovery after the 50 ms feedback watchdog. `ozzy_pcm_open` returns `-EPIPE` until replug, because `panic` is only cleared in `ozzy_pcm_stream_start`, which runs after open. Try stop / handshake / start on the next open or prepare.
- [ ] 5-minute soaks at 44.1 / 48 / 96 kHz (only 20 s each so far).
- [ ] Xone regression check of the core changes: period units (5b1a82e), `set_rate` errors, the abort lock and `sync_stop`.
- [ ] Deduplicate `common/devices/ploytec/dynacord_{pacer,feedback}.c` and `userspace/ploytec-play/{pacer,feedback}.c`, which have drifted apart.

## macOS userspace driver (OzzyDaemon, OzzyHAL, OzzyMIDI)

- [x] A failed `MIDIDeviceCreate` is retried on every tenth periodic poll check ("fix(macos): retry a failed MIDIDeviceCreate"). Build-tested only.
- [ ] IsoTooOld / IsoTooNew at 48 kHz. The bus already reseeds the start frame from `GetBusFrameNumber` after any iso error (`UserBus.cpp`, since 98ede90). What's left is that a request-level error sets the engine's `mFailed`, which costs a Poll restart of about 1.5 s. Check the logs on the machine in daily use (`~/Library/Logs/OzzyDaemon.log`, `log show --predicate 'subsystem == "OzzyDaemon" OR subsystem == "Ozzy"'`) before touching it. No occurrences closes this.
- [ ] MIDI bytes dropped until the device-online block has run after a MIDIServer restart (`PostDeviceOnline`).
- [ ] Duplicate `PropertiesChanged(NominalSampleRate)` from the HAL.
- [ ] Possible stale byte 0x00 on bulk IN after a rate change (full-scale clicks on in1/in3). Not seen since 2026-10-12; watch for it.

## Features

- [ ] System-wide input mute.
- [ ] Input volume, like the output volume (`macos/OzzyHAL/OzzyVolume.h`). This reverses an earlier decision against input volume, made because Slack/Zoom/Chrome automatic gain control would drive it. Settle that first, e.g. hide the control from apps or accept that they move it. Mute has no such conflict.

## Test tools

- [ ] Recover loopio / ratectl / midiloop / midimon / midiecho / flipper.sh from the Bash heredocs in session transcript 84cd2a6e-4a50-4b8f-bd02-87c7a977d680, and commit them under `macos/tests/tools/`.

## Parked: the macOS kext

The userspace driver replaced the kext for the CMS 600-3, so no more work goes into it unless that changes. Known open items: `kmutil unload` leaves an instance behind (`StopThread` returns early without logging); `OzzyUserClient::clientMemoryForType` reads the shm descriptor without synchronising against unmap; 0x86 overruns at 96 kHz; the uninstaller's `kmutil rebuild` path has only been stub-tested; the kext uninstaller half-removes a userspace install (the `com.ozzy.userspace` receipt check is now possible); plus the deferred review minors from the kext build.
