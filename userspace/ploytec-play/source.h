#ifndef PLOYTEC_PLAY_SOURCE_H
#define PLOYTEC_PLAY_SOURCE_H

#include <stddef.h>
#include <stdint.h>
#include "ploytec_defs.h"

/* Pack one frame of signed 24-bit samples (one per channel) as S24_3LE. */
void pack_s24(uint8_t *frame, const int32_t samples[DYNACORD_CHANNELS]);

/* Sine tone on one output channel (1..4), or on all of them (0). */
struct tone {
	double phase, step, amp;
	int channel;
};
void tone_init(struct tone *t, unsigned rate, double hz, double dbfs, int channel);
void tone_render(struct tone *t, uint8_t *frames, size_t n);

/*
 * WAV (or anything libsndfile reads) at exactly `rate`. Source channels 1-4
 * go to outputs 1-4 and any further channels are dropped; a mono file goes
 * to `mono_channel`. On failure returns NULL with a message in err.
 */
struct wav;
struct wav *wav_open(const char *path, unsigned rate, int mono_channel, char *err, size_t errlen);
size_t wav_render(struct wav *w, uint8_t *frames, size_t n);  /* 0 at end of file */
void wav_close(struct wav *w);

#endif
