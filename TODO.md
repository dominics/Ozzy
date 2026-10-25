# TODO

Open work on the Dynacord CMS 600-3 support, gathered on 2026-10-26 from three session handoffs: the kext follow-ups, the Linux ALSA port and its security review. Checked against `dynacord-cms600` at 5966e13; anything already landed has been dropped. Reverse-engineering TODOs stay in `~/workspace/dynacord-cms-pcap/TODO.md`.

## Linux ALSA driver (`linux/devices/dynacord.c`)

Playback, capture, two-port MIDI and unplug/replug all work on spork (kernel 6.8). Left to do:

- [ ] **Security review triage.** The background review of the 46577ac push flagged two categories in `dynacord.c`, labels only: (1) race / use-after-free, (2) untrusted device input. Nobody has looked at them yet, and the MIDI work (7548a01) has added more completion-handler code since. Candidates to confirm or rule out:
  - `dynacord_fill_out` / `dynacord_in_complete` call `snd_pcm_period_elapsed()` after dropping `sub->lock`, while `ozzy_pcm_close` can clear `sub->instance` in between. The URBs are device-owned and keep running after the trigger stops, so this window is wider than in the core's own handlers.
  - `dynacord_init` frees `priv` when a post-reset handshake fails, while MIDI and PCM paths can still reach `chip->private_data`. Also `dynacord_free` racing an in-flight `midi_out` completion.
  - `xchg()` on `bool` fields (`fb_lost`, `misaligned`); a plain `priv->running` read in `dynacord_set_rate` racing `dynacord_stop_urbs` on disconnect.
  - Device input looks bounded on a first read (the USB core caps `actual_length` at the buffer size, iso offsets are set by the driver, MIDI parsing loops on `actual_length`), but nobody has checked it properly.
  - Fix what is real as `git commit --fixup` against the commit that introduced it, build with `make -C linux` on spork (no Linux build on the Mac; don't load the module on dagger), checkpatch clean, and let the review re-run on the next push.
- [ ] **Recovery after the feedback watchdog.** Once the 50 ms no-feedback watchdog sets `panic`, `ozzy_pcm_open` returns `-EPIPE` until replug or module reload, because `panic` is only cleared in `ozzy_pcm_stream_start`. A fresh handshake probably re-arms the device, so try stop / handshake / start on the next open or prepare.
- [ ] **5-minute soaks** per rate (44.1 / 48 / 96 kHz) with `speaker-test -D hw:<card> -c 4 -r <rate> -F S24_3LE`. Only 20 s per rate has been run.
- [ ] **Xone regression check.** The period-units fix (5b1a82e) and the `set_rate` error return change the Xone path too, and haven't been tested on a Xone.
- [ ] **Deduplicate the pacer and feedback code.** `common/devices/ploytec/dynacord_{pacer,feedback}.c` began as copies of `userspace/ploytec-play/{pacer,feedback}.c` and have drifted apart since.

## macOS kext (`macos/Backends/OzzyKext`)

The userspace driver (OzzyDaemon) is now the supported path for the CMS 600-3, and the kext is no longer installed on dominic-macbook. Treat these as low priority unless the kext comes back for the CMS. Any kext testing needs a Recovery trip to lower SIP first, and the KDK for removal.

- [ ] **`kmutil unload` leaves an instance behind.** With a stream running, unload logs `stop` and an aborted iso pipe, then nothing: no `free`, and "class OzzyKext has 1 instance". `StopThread` returns early on `mTransfersLeaked` without logging. Next step: log at every `StopThread` exit and at the end of `Teardown` with the drain result, then test the unload path (no `willTerminate`) with the stream running and idle.
- [ ] **`OzzyUserClient::clientMemoryForType`** reads the owner's shm descriptor without synchronising against `UnmapSharedMemory` (a narrow use-after-free window), and `mOwner` is not retained.
- [ ] **Bulk 0x86 NotResponding / Overrun at 96 kHz.** The Poll restart recovers in about 1.5 s.
- [ ] **MIDI bytes dropped after a MIDIServer restart.** One loop round-trip came back empty. OzzyMIDI drops bytes until the device-online block has run (`PostDeviceOnline`).
- [ ] **A failed `MIDIDeviceCreate` is not retried** (`EnsureMIDIDeviceCreated` logs and gives up).
- [ ] **Duplicate `PropertiesChanged(NominalSampleRate)`** from the HAL.
- [ ] **Boot-time kext log lines missing** at first light.
- [ ] **Long sleep that re-enumerates the mixer**: untested, though unplug + replug passed.
- [ ] **The uninstaller's `kmutil rebuild` path** has only been stub-tested: the real rebuild, its exit code on an approval timeout, and `kmutil inspect` between rebuild and reboot.
- [ ] **The kext uninstaller half-removes a userspace install.** `uninstall.command` deletes `/usr/local/bin/OzzyDaemon` and the HAL/MIDI drivers but leaves the userspace LaunchAgent behind. The `.pkg` with receipt `com.ozzy.userspace` now exists, so this is no longer blocked: if `pkgutil --pkg-info com.ozzy.userspace` succeeds, `uninstall.command` should refuse (or hand over to `/Library/Application Support/Ozzy/uninstall.sh`), and `install.command` option 1 should refuse until the package is removed. Needs Dominic's go-ahead.
- [ ] **dominic-rilla install** (ask first; nothing is installed there). Check `profiles status -type enrollment` (MDM may forbid Reduced Security or kext loading), `uname -m`, and that the 26.5 SDK is present. Anything done there is visible at work.
- [ ] **Deferred review minors** from the kext build. The ones still worth acting on:
  - `dynacord_pacer.c:109` subtracts before the `< 0` guard (INT_MIN overflow).
  - Seqlock writes are volatile-only, with no release barrier; check HAL reader and writer ordering on arm64.
  - The completion ctx index and the `frameCount` reported by the bus aren't bounds-checked.
  - The `mRefFrame` / `mRefHost` reference pair is never refreshed, though the comment says "latest".
  - A queue-full drop leaks an iso slot and loses its callback; the isoch completion lacks a null check.
  - The MIDI parser edge cases have been traced but have no tests: realtime bytes inside SysEx, an F0 interrupting SysEx, a stray F7, SysEx of exactly 256 bytes.
  - `ConfigureEntities` can run concurrently from `Send` and the poll thread.
  - `StopIO` decides which client is last from a single load.

## macOS userspace driver (OzzyDaemon)

- [ ] **Occasional IsoTooOld / IsoTooNew self-restarts at 48 kHz.** Candidate fix: reseed `nextFrame` from the bus frame number instead of carrying it forward (`UserBus.cpp`, iso submit).
- [ ] **Possible stale byte from the mixer on bulk IN.** In some 48 kHz sessions started right after a rate change, byte 0x00 of the 64-byte IN frame repeated the previous frame's value in about 8% of frames. That byte holds bit 23 of in1 and in3, so quiet signals there decode as full-scale clicks. It hasn't been reproduced since, and a later rate change cleared it. Suspect this first if in1/in3 click.

## Test tools

- [ ] The loopio / ratectl / midiloop / midimon / midiecho / flipper.sh test tools were never committed. Their sources can be recovered from the Bash heredocs in session transcript 84cd2a6e-4a50-4b8f-bd02-87c7a977d680. Worth committing under `macos/tests/tools/` if more hardware testing is coming.
