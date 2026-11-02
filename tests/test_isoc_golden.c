/* SPDX-License-Identifier: MIT */
/*
 * CMS 600-3 output pacing against the Windows driver (common/isoc_feedback.h): the
 * feedback the Windows driver received on 0x81 goes through isoc_fb as the engine
 * feeds it (one 3-byte packet per 1 ms, four packets per transfer), and the frames
 * isoc_fb gives each OUT microframe are compared with what the Windows driver sent
 * over the same span (fixtures/dynacord_counter_*.h, at 44.1, 48 and 96 kHz):
 *
 *   - the same total frames, to within 1 ms of audio;
 *   - a running difference of at most 3 frames at any point;
 *   - per-size packet counts within 1% (at least 8).
 *
 * Each rate runs with the feedback written ahead of the output side (the ring's same
 * lap) and behind it by the engine's OUT queues (3 and 6 transfers of 32 microframes,
 * plus one transfer of slack), and no feedback packet may count as wrong.
 */

#include "check.h"
#include "common/isoc_feedback.h"
#include "common/devices/ploytec/ploytec_defs.h"
#include "fixtures/dynacord_counter_44100.h"
#include "fixtures/dynacord_counter_48000.h"
#include "fixtures/dynacord_counter_96000.h"
#include <stdlib.h>

#define FB_INTERVAL   4    /* bInterval of the CMS 600-3's feedback endpoint 0x81: 8 microframes */
#define MAX_SIZE      16   /* frames in one microframe's packet, exclusive */

/* Feedback written this many microframes behind the output side (negative: ahead) */
static const long lags[] = {
	-120,
	0,
	ISOC_OUT_TRANSFERS * ISOC_TRANSFER_UFRAMES,
	(ISOC_OUT_TRANSFERS_DEEP + 1) * ISOC_TRANSFER_UFRAMES,
};

static void golden(uint32_t rate, const unsigned char *fb, size_t nfb, const unsigned char *win, size_t nwin,
		   long lag)
{
	const struct isoc_fb_config cfg = PLOYTEC_ISOC_FEEDBACK;
	uint32_t per_packet = isoc_fb_interval_uframes(FB_INTERVAL);
	uint32_t per_transfer = isoc_fb_packets(per_packet);
	long ours[MAX_SIZE] = { 0 }, theirs[MAX_SIZE] = { 0 }, total_ours = 0, total_theirs = 0, drift = 0;
	size_t npk = nfb * per_packet < nwin ? nfb * per_packet : nwin;
	size_t i, g = 0;
	struct isoc_fb f;
	int s;

	CHECK(isoc_fb_init(&f, rate, &cfg, FB_INTERVAL), "%u Hz: feedback interval refused", rate);
	for (i = 0; i < npk; i++) {
		uint32_t o, t = win[i];
		long d;

		/* A feedback transfer completes once its last microframe is lag microframes behind */
		while (g < nfb && (long)((g + per_transfer) * per_packet) <= (long)i - lag) {
			uint32_t k;

			for (k = 0; k < per_transfer && g < nfb; k++, g++) {
				uint8_t pkt[PLOYTEC_ISOC_FB_PACKET_SIZE] = { fb[g] };

				isoc_fb_packet(&f, pkt, sizeof(pkt));
			}
		}
		o = isoc_fb_out_frames(&f, 1);
		CHECK(o < MAX_SIZE && t < MAX_SIZE, "%u Hz: packet %zu of %u frames (windows %u)", rate, i, o, t);
		if (o >= MAX_SIZE || t >= MAX_SIZE)
			return;
		ours[o]++;
		theirs[t]++;
		total_ours += o;
		total_theirs += t;
		d = labs(total_ours - total_theirs);
		if (d > drift)
			drift = d;
	}
	fprintf(stderr, "%u Hz, feedback %ld microframes behind: %zu packets, ours %ld frames, windows %ld, "
		"max running difference %ld\n", rate, lag, npk, total_ours, total_theirs, drift);
	CHECK(labs(total_ours - total_theirs) <= (long)(rate / 1000 + 2), "%u Hz lag %ld: totals %ld vs %ld",
	      rate, lag, total_ours, total_theirs);
	CHECK(drift <= 3, "%u Hz lag %ld: running difference reached %ld frames", rate, lag, drift);
	for (s = 0; s < MAX_SIZE; s++) {
		long tol = theirs[s] / 100 > 8 ? theirs[s] / 100 : 8;

		CHECK(labs(ours[s] - theirs[s]) <= tol, "%u Hz lag %ld: %d-frame packets, ours %ld windows %ld",
		      rate, lag, s, ours[s], theirs[s]);
	}
	CHECK(f.errors == 0 && !f.broken, "%u Hz lag %ld: %u feedback packets counted wrong", rate, lag,
	      f.errors);
}

int main(void)
{
	size_t l;

	for (l = 0; l < sizeof(lags) / sizeof(lags[0]); l++) {
		golden(44100, fb_44100, sizeof(fb_44100), out_44100, sizeof(out_44100), lags[l]);
		golden(48000, fb_48000, sizeof(fb_48000), out_48000, sizeof(out_48000), lags[l]);
		golden(96000, fb_96000, sizeof(fb_96000), out_96000, sizeof(out_96000), lags[l]);
	}
	return CHECK_DONE("isoc golden");
}
