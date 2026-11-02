#ifndef PLOYTEC_PLAY_METER_H
#define PLOYTEC_PLAY_METER_H

#include <stddef.h>
#include <stdint.h>

#define METER_CHANNELS 4
#define METER_FRAME_SIZE 64
/* Reported for a channel whose peak is 0; one LSB of 24-bit is -138.5 dBFS. */
#define METER_FLOOR_DB -144.0

struct meter {
	int32_t peak[METER_CHANNELS];   /* largest |sample| seen on each input channel */
	unsigned long long frames;      /* frames decoded into peak */
	unsigned long long misaligned;  /* frames skipped because the framing bits were wrong */
};

/*
 * Decode the 64-byte bulk IN frames in buf and raise the per-channel peaks.
 * A frame whose fixed bits don't match (bits 2-7 of each slice byte set,
 * 0xce at 0x1b and 0x3b) is counted as misaligned and not decoded, as is a
 * trailing partial frame.
 */
void meter_feed(struct meter *m, const uint8_t *buf, size_t len);

/* Peak magnitude in dBFS relative to 2^23, or METER_FLOOR_DB for 0. */
double meter_dbfs(int32_t peak);

#endif
