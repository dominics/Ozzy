# ploytec-play

A userspace test player for the **Dynacord CMS 600-3** (the old hardware revision with a Ploytec USB card, `0562:03eb`). It plays a sine tone or a WAV file to the mixer's 4 USB outputs using libusb, without any kernel driver, so on macOS it works with SIP enabled.

This is a protocol prototype, not a sound-card driver: the mixer does not appear as an audio device. Protocol notes and the captures behind them are in [dominics/dynacord-cms-pcap](https://github.com/dominics/dynacord-cms-pcap) (`FINDINGS.md`).

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
- `--verbose`: log libusb and the control-request exchange

## Power-cycle quirk

The mixer's USB card only enumerates if the mixer is powered on with the USB cable already connected. If the tool says the mixer isn't found, power-cycle the mixer with the cable plugged in.

## Reading the stats line

Once a second:

```
t=  5.0s frames=480000 F=95.996 pkts 11:4 12:7996 underruns=0 iso_err=0 fb_bad=0 in=11059200B
```

- `frames`: total frames sent
- `F`: mean frames per ms the mixer reported consuming over the last second (nominal 44.1 / 48 / 96)
- `pkts N:count`: iso OUT packets of N frames sent in the last second
- `underruns`: packets that ran short because the source couldn't keep up
- `iso_err`: iso OUT packets that failed
- `fb_bad`: feedback readings ignored for being more than 2 away from nominal
- `in`: bytes read from the input endpoint (read only to keep the mixer streaming, then discarded)

A healthy run has `underruns=0` and `iso_err=0`.
