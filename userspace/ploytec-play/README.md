# ploytec-play

A userspace test player for the **Dynacord CMS 600-3** (the old hardware revision with a Ploytec USB card, `0562:03eb`). It plays a sine tone or a WAV file to the mixer's 4 USB outputs using libusb, without any kernel driver, so on macOS it works with SIP enabled.

This is a protocol prototype, not a sound-card driver: the mixer does not appear as an audio device.

## Build

macOS:

```sh
brew install libusb libsndfile pkgconf
make
make test
```

Linux needs `libusb-1.0-0-dev`, `libsndfile1-dev` and `pkg-config`, and either root or a udev rule for `0562:03eb`.

## Usage

```sh
./ploytec-play --rate 96000 --tone 440 --channel 1 --seconds 10
./ploytec-play --rate 48000 --channel all --level -20
./ploytec-play --rate 44100 --wav song.wav
```

- `--rate`: 44100, 48000 (default) or 96000
- `--tone HZ`: sine frequency (default 1000 Hz when there's no `--wav`)
- `--wav FILE`: anything libsndfile reads, at exactly `--rate` (no resampling). Channels 1-4 go to outputs 1-4, further channels are dropped, and mono goes to `--channel`.
- `--channel N|all`: output for the tone or a mono WAV (default `all`)
- `--level DBFS`: tone level (default -12)
- `--seconds N`: stop after N seconds; otherwise runs until Ctrl-C or the end of the WAV
- `--out-xfers N`: iso OUT transfers kept queued, 3 ms each (default 8, so 24 ms of output latency). The mixer stops streaming for good if this queue ever runs dry, so a value below 4 is likely to halt.
- `--verbose`: log libusb and the control-request exchange

## Real-time threads on macOS

Ordinary threads on macOS can go unscheduled for 10-30 ms at a time, longer than a short OUT queue lasts. USB completions pass through two threads: libusb's internal `org.libusb.device-hotplug` thread, which receives them from IOKit, and ours, which runs the callbacks. Both are given a Mach time-constraint policy at start-up, the same kind Core Audio uses for its IO threads. With it the worst callback gap measured was 4.5 ms. Promoting only one of the two isn't enough: with only libusb's thread the mixer halted at a 12 ms queue, and with only ours it survived but with gaps of up to 9.4 ms, right at the limit a 12 ms queue can absorb.

## Power-cycle quirk

The mixer's USB card only enumerates if the mixer is powered on with the USB cable already connected. If the tool says the mixer isn't found, power-cycle the mixer with the cable plugged in.

## Reading the stats line

Once a second:

```
t=  5.0s frames=480000 F=95.996 pkts 11:4 12:7996 underruns=0 iso_err=0 fb_bad=0 in=11059200B in dBFS: -138.5 -138.5 -138.5 -138.5
```

- `frames`: total frames sent
- `F`: mean frames per ms the mixer reported consuming over the last second (nominal 44.1 / 48 / 96)
- `pkts N:count`: iso OUT packets of N frames sent in the last second
- `underruns`: packets that ran short because the source couldn't keep up
- `iso_err`: iso OUT packets that failed
- `fb_bad`: feedback readings ignored for being more than 2 away from nominal
- `in`: bytes read from the input endpoint
- `in dBFS`: peak level of USB inputs 1-4 over the last second; -138.5 is one LSB, the idle noise floor, and -144.0 means exactly zero
- `misaligned`: input frames skipped because their fixed framing bits were wrong, shown only when non-zero

A healthy run has `underruns=0`, `iso_err=0` and no `misaligned`.
