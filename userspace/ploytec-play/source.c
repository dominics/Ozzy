#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sndfile.h>
#include "source.h"

void pack_s24(uint8_t *frame, const int32_t samples[DYNACORD_CHANNELS])
{
	for (int c = 0; c < DYNACORD_CHANNELS; c++) {
		uint32_t v = (uint32_t)samples[c];
		frame[3 * c] = v & 0xff;
		frame[3 * c + 1] = (v >> 8) & 0xff;
		frame[3 * c + 2] = (v >> 16) & 0xff;
	}
}

void tone_init(struct tone *t, unsigned rate, double hz, double dbfs, int channel)
{
	t->phase = 0.0;
	t->step = 2.0 * M_PI * hz / rate;
	t->amp = pow(10.0, dbfs / 20.0) * 8388607.0;
	t->channel = channel;
}

void tone_render(struct tone *t, uint8_t *frames, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		int32_t v = (int32_t)lrint(t->amp * sin(t->phase));
		int32_t s[DYNACORD_CHANNELS];
		for (int c = 0; c < DYNACORD_CHANNELS; c++)
			s[c] = (t->channel == 0 || t->channel == c + 1) ? v : 0;
		pack_s24(frames + i * DYNACORD_OUT_FRAME_SIZE, s);
		t->phase = fmod(t->phase + t->step, 2.0 * M_PI);
	}
}

struct wav {
	SNDFILE *sf;
	int channels;
	int mono_channel;
	int *buf;
	size_t buf_frames;
};

struct wav *wav_open(const char *path, unsigned rate, int mono_channel, char *err, size_t errlen)
{
	SF_INFO info = { 0 };
	SNDFILE *sf = sf_open(path, SFM_READ, &info);
	if (!sf) {
		snprintf(err, errlen, "%s: %s", path, sf_strerror(NULL));
		return NULL;
	}
	if ((unsigned)info.samplerate != rate) {
		snprintf(err, errlen, "%s is %d Hz but --rate is %u; resample it first",
			 path, info.samplerate, rate);
		sf_close(sf);
		return NULL;
	}
	struct wav *w = calloc(1, sizeof(*w));
	w->sf = sf;
	w->channels = info.channels;
	w->mono_channel = mono_channel >= 1 && mono_channel <= DYNACORD_CHANNELS ? mono_channel : 1;
	return w;
}

size_t wav_render(struct wav *w, uint8_t *frames, size_t n)
{
	if (w->buf_frames < n) {
		free(w->buf);
		w->buf = malloc(n * (size_t)w->channels * sizeof(int));
		w->buf_frames = n;
	}
	sf_count_t got = sf_readf_int(w->sf, w->buf, (sf_count_t)n);
	for (sf_count_t i = 0; i < got; i++) {
		int32_t s[DYNACORD_CHANNELS] = { 0 };
		const int *in = w->buf + i * w->channels;
		if (w->channels == 1) {
			s[w->mono_channel - 1] = in[0] >> 8;
		} else {
			for (int c = 0; c < DYNACORD_CHANNELS && c < w->channels; c++)
				s[c] = in[c] >> 8;
		}
		pack_s24(frames + i * DYNACORD_OUT_FRAME_SIZE, s);
	}
	return (size_t)got;
}

void wav_close(struct wav *w)
{
	sf_close(w->sf);
	free(w->buf);
	free(w);
}
