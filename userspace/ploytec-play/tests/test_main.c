#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sndfile.h>
#include "test.h"
#include "../feedback.h"
#include "../pacer.h"
#include "../ring.h"
#include "../source.h"
#include "../packetize.h"
#include "../xfer.h"

int test_failures;

static void test_feedback(void)
{
	const uint8_t steady[] = { 0x60, 0x60, 0x60 };
	const uint8_t newest_45[] = { 0x2d, 0x2c, 0x2c };
	const uint8_t older_45[] = { 0x2c, 0x2d, 0x2c };

	CHECK_EQ(feedback_parse(steady, 3), 96);
	CHECK_EQ(feedback_parse(newest_45, 3), 45);
	CHECK_EQ(feedback_parse(older_45, 3), 44);
	CHECK_EQ(feedback_parse(steady, 0), -1);
	CHECK_EQ(feedback_parse(steady, 1), 96);
}

static void test_pacer_nominal(void)
{
	static const unsigned rates[] = { 44100, 48000, 96000 };
	for (unsigned r = 0; r < 3; r++) {
		struct pacer p;
		pacer_init(&p, rates[r]);
		long total = 0;
		int lo = 99, hi = 0;
		for (int i = 0; i < 8000; i++) {
			int n = pacer_next(&p);
			total += n;
			if (n < lo) lo = n;
			if (n > hi) hi = n;
		}
		CHECK_EQ(total, rates[r]);
		CHECK(hi - lo <= 1);
	}
}

static void test_pacer_spread(void)
{
	for (int total = 0; total <= 8 * 13; total++) {
		int sum = 0;
		for (int slot = 0; slot < 8; slot++) {
			int n = pacer_spread(total, slot);
			sum += n;
			CHECK(n >= total / 8);
			CHECK(n <= (total + 7) / 8);
		}
		CHECK_EQ(sum, total);
	}
}

static void test_pacer_feedback(void)
{
	struct pacer p;
	pacer_init(&p, 96000);

	/* A short millisecond gives exactly one 11-frame packet in the next group. */
	pacer_feedback(&p, 95);
	int sum = 0, elevens = 0;
	for (int i = 0; i < 8; i++) {
		int n = pacer_next(&p);
		sum += n;
		elevens += n == 11;
	}
	CHECK_EQ(sum, 95);
	CHECK_EQ(elevens, 1);

	/* Out-of-range and negative readings are ignored and counted. */
	pacer_feedback(&p, 96);
	pacer_feedback(&p, 200);
	pacer_feedback(&p, -1);
	pacer_feedback(&p, 0);
	CHECK_EQ(p.invalid, 3);
	sum = 0;
	for (int i = 0; i < 8; i++)
		sum += pacer_next(&p);
	CHECK_EQ(sum, 96);

	/* Feedback that arrives mid-group takes effect at the next group. */
	pacer_init(&p, 48000);
	for (int i = 0; i < 3; i++)
		CHECK_EQ(pacer_next(&p), 6);
	pacer_feedback(&p, 47);
	sum = 0;
	for (int i = 0; i < 5; i++)
		sum += pacer_next(&p);
	CHECK_EQ(sum, 30);
	sum = 0;
	for (int i = 0; i < 8; i++)
		sum += pacer_next(&p);
	CHECK_EQ(sum, 47);
}

static void test_pacer_feedback_burst(void)
{
	/* Feedback arrives 5 readings at a time; each must pace its own group. */
	static const int burst[5] = { 48, 47, 48, 48, 47 };
	struct pacer p;
	pacer_init(&p, 48000);
	for (int i = 0; i < 5; i++)
		pacer_feedback(&p, burst[i]);
	for (int g = 0; g < 5; g++) {
		int sum = 0;
		for (int i = 0; i < 8; i++)
			sum += pacer_next(&p);
		CHECK_EQ(sum, burst[g]);
	}
	/* With nothing queued, the last reading carries on. */
	int sum = 0;
	for (int i = 0; i < 8; i++)
		sum += pacer_next(&p);
	CHECK_EQ(sum, 47);
}

static void test_pack_walk(void)
{
	/* Matches captures/cms-walk-96000: bit b of channel c is byte 3c + b/8, bit b%8. */
	for (int c = 0; c < 4; c++) {
		for (int b = 0; b < 24; b++) {
			int32_t s[4] = { 0, 0, 0, 0 };
			uint8_t f[12];
			s[c] = (int32_t)(b == 23 ? -8388608 : (1 << b));
			pack_s24(f, s);
			for (int i = 0; i < 12; i++)
				CHECK_EQ(f[i], i == 3 * c + b / 8 ? 1 << (b % 8) : 0);
		}
	}
}

static void test_ring(void)
{
	struct ring r;
	uint8_t in[12 * 5], out[12 * 5];
	for (int i = 0; i < 60; i++)
		in[i] = (uint8_t)i;

	CHECK_EQ(ring_init(&r, 8), 0);
	CHECK_EQ(ring_space(&r), 8);
	CHECK_EQ(ring_write(&r, in, 5), 5);
	CHECK_EQ(ring_read(&r, out, 3), 3);
	CHECK(memcmp(out, in, 36) == 0);
	CHECK_EQ(ring_write(&r, in, 5), 5);    /* wraps */
	CHECK_EQ(ring_fill(&r), 7);
	CHECK_EQ(ring_write(&r, in, 5), 1);    /* only 1 frame of space */
	CHECK_EQ(ring_read(&r, out, 5), 5);
	CHECK(memcmp(out, in + 36, 24) == 0);
	CHECK(memcmp(out + 24, in, 36) == 0);
	ring_free(&r);
}

static int32_t s24_at(const uint8_t *f, int c)
{
	int32_t v = f[3 * c] | f[3 * c + 1] << 8 | f[3 * c + 2] << 16;
	return v & 0x800000 ? v - 0x1000000 : v;
}

static void test_tone(void)
{
	struct tone t;
	uint8_t f[48 * 12];
	tone_init(&t, 48000, 1000.0, 0.0, 2);
	tone_render(&t, f, 48);
	int32_t peak = 0;
	for (int i = 0; i < 48; i++) {
		CHECK_EQ(s24_at(f + 12 * i, 0), 0);
		CHECK_EQ(s24_at(f + 12 * i, 2), 0);
		CHECK_EQ(s24_at(f + 12 * i, 3), 0);
		int32_t v = s24_at(f + 12 * i, 1);
		if (abs(v) > peak) peak = abs(v);
	}
	CHECK(peak > 8300000);   /* one full cycle at 0 dBFS */
	CHECK_EQ(s24_at(f, 1), 0);  /* starts at phase 0 */
}

static void write_wav(const char *path, int rate, int channels, int frames)
{
	SF_INFO info = { .samplerate = rate, .channels = channels,
			 .format = SF_FORMAT_WAV | SF_FORMAT_PCM_24 };
	SNDFILE *sf = sf_open(path, SFM_WRITE, &info);
	int *buf = calloc((size_t)frames * channels, sizeof(int));
	for (int i = 0; i < frames; i++)
		for (int c = 0; c < channels; c++)
			buf[i * channels + c] = (c + 1) << 16 << 8;   /* 24-bit value (c+1)<<16 */
	sf_writef_int(sf, buf, frames);
	sf_close(sf);
	free(buf);
}

static void test_wav(void)
{
	char err[256];
	uint8_t f[10 * 12];
	struct wav *w;

	write_wav("/tmp/ploytec-play-test-6ch.wav", 48000, 6, 10);
	w = wav_open("/tmp/ploytec-play-test-6ch.wav", 48000, 1, err, sizeof(err));
	CHECK(w != NULL);
	if (w) {
		CHECK_EQ(wav_render(w, f, 10), 10);
		for (int c = 0; c < 4; c++)
			CHECK_EQ(s24_at(f + 12 * 9, c), (c + 1) << 16);
		CHECK_EQ(wav_render(w, f, 10), 0);
		wav_close(w);
	}

	write_wav("/tmp/ploytec-play-test-mono.wav", 48000, 1, 4);
	w = wav_open("/tmp/ploytec-play-test-mono.wav", 48000, 3, err, sizeof(err));
	CHECK(w != NULL);
	if (w) {
		CHECK_EQ(wav_render(w, f, 4), 4);
		CHECK_EQ(s24_at(f, 0), 0);
		CHECK_EQ(s24_at(f, 2), 1 << 16);
		wav_close(w);
	}

	w = wav_open("/tmp/ploytec-play-test-mono.wav", 96000, 1, err, sizeof(err));
	CHECK(w == NULL);
	CHECK(strstr(err, "48000") != NULL);

	w = wav_open("/tmp/does-not-exist.wav", 48000, 1, err, sizeof(err));
	CHECK(w == NULL);
}

static void test_fill_packets(void)
{
	/* 10 frames available at 48 kHz; 3 packets of 6 frames want 18. */
	static const enum fill_mode modes[] = { FILL_NORMAL, FILL_DRAINING, FILL_SILENCE };
	static const int want_underruns[] = { 2, 0, 0 };
	for (int m = 0; m < 3; m++) {
		struct pacer p;
		struct ring r;
		struct fill_result res = { 0 };
		uint8_t frame[12], buf[3 * 13 * 12];
		unsigned lengths[3];
		memset(frame, 0xab, sizeof(frame));
		pacer_init(&p, 48000);
		ring_init(&r, 32);
		for (int i = 0; i < 10; i++)
			ring_write(&r, frame, 1);

		int bytes = fill_packets(&p, &r, buf, lengths, 3, modes[m], &res);

		CHECK_EQ(bytes, 18 * 12);
		CHECK_EQ(lengths[0], 72);
		CHECK_EQ(lengths[2], 72);
		CHECK_EQ(res.frames, 18);
		CHECK_EQ(res.hist[6], 3);
		CHECK_EQ(res.underruns, want_underruns[m]);
		CHECK_EQ(buf[9 * 12], modes[m] == FILL_SILENCE ? 0 : 0xab);   /* 10th frame */
		CHECK_EQ(buf[10 * 12], 0);                                     /* 11th: zero-filled */
		CHECK_EQ(ring_fill(&r), modes[m] == FILL_SILENCE ? 10 : 0);
		ring_free(&r);
	}
}

static void test_xfer_action(void)
{
	CHECK_EQ(xfer_action(LIBUSB_TRANSFER_COMPLETED, 0), XFER_RESUBMIT);
	CHECK_EQ(xfer_action(LIBUSB_TRANSFER_TIMED_OUT, 0), XFER_RESUBMIT);
	CHECK_EQ(xfer_action(LIBUSB_TRANSFER_ERROR, 0), XFER_RESUBMIT);
	CHECK_EQ(xfer_action(LIBUSB_TRANSFER_NO_DEVICE, 0), XFER_FAIL);
	/* Cancelled while we weren't stopping: the pipe was aborted under us. */
	CHECK_EQ(xfer_action(LIBUSB_TRANSFER_CANCELLED, 0), XFER_FAIL);
	CHECK_EQ(xfer_action(LIBUSB_TRANSFER_CANCELLED, 1), XFER_RETIRE);
	CHECK_EQ(xfer_action(LIBUSB_TRANSFER_COMPLETED, 1), XFER_RETIRE);
	CHECK_EQ(xfer_action(LIBUSB_TRANSFER_NO_DEVICE, 1), XFER_RETIRE);
}

int main(void)
{
	test_feedback();
	test_pacer_nominal();
	test_pacer_spread();
	test_pacer_feedback();
	test_pacer_feedback_burst();
	test_pack_walk();
	test_ring();
	test_tone();
	test_wav();
	test_fill_packets();
	test_xfer_action();
	if (test_failures) {
		fprintf(stderr, "%d failure(s)\n", test_failures);
		return 1;
	}
	printf("all tests passed\n");
	return 0;
}
