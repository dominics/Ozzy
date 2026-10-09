# Ploytec RE — Open Questions and Leads

## Open Questions

### Sample rate change: 5-call dance vs official driver
Our driver sends SET_CUR 5 times alternating 0x86/0x05 (from Windows USB capture). The official
macOS driver only calls `setFrequency` once per pipe (2 total: input + output). The 5-call
pattern may be a Windows driver quirk, or redundant. Need to test if 2 calls works for all devices.

### Junction encoder vs our bit-interleaved codec
`pcmTo24Junction6CH` does **channel reordering** (0→0, 3→1, 1→2, 4→3, 2→4, 5→5) and
extracts 3 bytes from 4-byte int samples. It does NOT do bit-level interleaving.
Our codec (`ploytec_codec.h`) does bit-interleaved encoding. Need to verify these produce
the same wire format, or if our codec is wrong.

### Index 0 bits 3-7 (0x08–0x80)
- For product 0x644: bits 3-4 (mask `0x18`) come from `this[0xd2]`, rest preserved with `& 0xE7`
- ESU uses fixed composite states (0x32, 0xB0, 0xB2) rather than individual flags
- Bits 0-1 now understood (USB1 mode and clock source), but upper bits still device-dependent

### PGNoiseGenerator anti-tamper
`onServiceStart` uses a PRNG seeded with VID/PID to verify expected values. This is a
license/anti-clone check. Not relevant to our driver but interesting.

### AJ subsystem
`AJ::configurationDone`, `AJ::sendCPort`, `AJ::getLevelInfo` — purpose unclear. May be
related to level metering or some control protocol. Has its own pipe assignment via `AJ::assignPipes`.

### CMS 600-3: one bulk IN 0x86 NotResponding at 96 kHz
In the first 96 kHz OzzyDaemon run (IOUSBLib, 2026-10-09) one bulk read on 0x86 completed with
0xe00002ed (kIOReturnNotResponding) about 4.4 s in, then the pipe recovered on the next read with
no restart. Not seen at 48 kHz. Unknown whether the mixer NAKed past the transfer timeout or the
host controller dropped it; needs a longer 96 kHz soak to see if it repeats.

### CMS 600-3: input frame byte 0x00 one frame late in some sessions
Seen 2026-10-12 through OzzyDaemon at 48 kHz, in a run of sessions started right after a rate change
(96 -> 48 and once 44.1 -> 48, 18:48-18:55), then not again in 40+ sessions or after 5 min at 96 kHz.
In about 8% of frames, byte 0x00 of the 64-byte IN frame carries the previous frame's value. That byte
holds bit 23 of channels 1 and 3 (bits 0 and 1), so near-zero samples on those inputs decode as full
scale; bit 23 of channels 2 and 4 is in byte 0x20, which was never late. In frames where both inputs
changed sign, 88% of channel 1 hits had a channel 3 hit too, so the whole byte is late, with no
periodicity by USB transfer. The daemon reads bulk IN zero-copy into the ring and the HAL decodes whole
frames, so the late byte is in the USB data from the mixer. Unknown: whether the rate-change control
sequence (see the 5-call SET_CUR question above) puts the mixer into this state, and whether a daemon
restart or power cycle clears it. A later rate change did.

## Resolved (moved from questions)

- ~~CMS 600-3 has no interfaces from userspace~~ - With no kernel driver matching it, macOS leaves the
  vendor-class device unconfigured, so IOUSBLib finds 0 interfaces. A userspace driver must
  `USBDeviceOpen` and `SetConfiguration(1)` itself before opening interfaces 0 and 1 (alt 1).

- ~~Vendor request 'A'~~ — Now understood: ESU clock config (wIndex=0x101) and Wolfson codec (wIndex=0x102/0x106)
- ~~Index 2 digital output selector~~ — Write-only, `AjExtData[4] & 0xFFFF`, only for devices with `this[0x7AC]`
- ~~setEsuCpldByte~~ — Writes to 'I' wIndex=1 with `| 0xE7` mask
- ~~this[0x1888] meaning~~ — USB 2.0 High Speed flag, NOT bulk device flag
- ~~wValue high byte~~ — Sign extension artifact from `(short)(char)byte` cast

## Leads

- **pcmTo24Junction8CH** — decompile to verify our 8-channel codec against the official encoder
- **USB::buildDummyDescriptor** — how the driver synthesizes USB audio descriptors for Ploytec devices
- **sendElektronChannelMap** — USB transactions for Elektron channel config (for Elektron support)
- **rtsProcessBulkOut** — the actual real-time audio processing loop (partially decompiled, shows MIDI embedding)
- **Real-time service functions**: `rtsBulkOutBulkIn`, `rtsInterruptOutInterruptIn` — the streaming state machines
- Windows driver RE for sample rate change pattern comparison
