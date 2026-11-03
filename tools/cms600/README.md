# cms600

Hardware test tools for the Dynacord CMS 600-3 under the macOS userspace driver (OzzyDaemon, OzzyHAL, OzzyMIDI). They are what the 0.4.x release checks in `TODO.md` were run with. For upstream's devices and the kext, see `tools/hwtest`.

Set the mixer up as a loopback first: outputs 1-2 and 3-4 back into inputs 1-2, and a MIDI cable from OUT to IN on both ports. Record the USB input rather than listening to it.

| Tool | What it does | Build |
|---|---|---|
| `duplex.swift` | Sets a CoreAudio device's rate, plays 997 Hz on output 1 and 1499 Hz on output 3 (-36 dBFS), records all inputs to a float WAV, and counts IO cycles and processor overloads | `swiftc -O -o duplex duplex.swift` |
| `analyse.py` | Fits both tones per 1024-sample block of a duplex WAV, and reports each input's level and the blocks where the residual comes within 20 dB of the signal (glitches) | Python 3, numpy |
| `midiport.swift` | Lists CoreMIDI endpoints, or sends a fixed stream (`loop`) or a set of non-commercial SysEx messages (`sysex`) to the port whose name contains a substring, and checks that the same bytes come back | `swiftc -O -o midiport midiport.swift` |
| `loopback.sh` | The release run: 30 s duplex at 44.1, 48 and 96 kHz with both MIDI ports looping during the audio, 300 s at 48 kHz, a minute idle, then the daemon log lines since the start | builds the two Swift tools on first use |

```sh
./duplex DYNACORD 48000 30 d48.wav && python3 analyse.py d48.wav
./midiport loop USB-MIDI
./midiport sysex SystemCtrl   # SystemCtrl drives the FX processors: only non-commercial (0x7D) SysEx
./loopback.sh                 # takes /tmp/ploytec-hw.lock for the run
```

Clean: no overloads, 0 glitch blocks on the inputs carrying the tones, and `identical: true` from each MIDI loop. The first 0.5 s of each WAV is skipped, as it covers the device start.
