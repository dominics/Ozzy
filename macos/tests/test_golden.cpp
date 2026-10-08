/* SPDX-License-Identifier: MIT */
#include <cstddef>
#include "../../common/devices/ploytec/dynacord_pacer.h"
#include "../../userspace/ploytec-play/tests/fixtures/counter_44100.h"
#include "../../userspace/ploytec-play/tests/fixtures/counter_48000.h"
#include "../../userspace/ploytec-play/tests/fixtures/counter_96000.h"
#include "golden_compare.h"
#include "tests.h"

/* Feed the feedback Windows received through the shared pacer, in bursts of
 * 5 readings every 40 packets as the stream delivers it. */
static void golden(unsigned rate, const unsigned char* fb, size_t nfb, const unsigned char* win, size_t nwin)
{
	struct pacer p;
	GoldenTally tally;
	size_t npk = nfb * 8 < nwin ? nfb * 8 : nwin;

	pacer_init(&p, rate);
	for (size_t i = 0; i < npk; i++) {
		if (i % (8 * 5) == 0)
			for (size_t g = i / 8; g < i / 8 + 5 && g < nfb; g++)
				pacer_feedback(&p, fb[g]);
		tally.add(pacer_next(&p), win[i]);
	}
	tally.check(rate, 3);
	CHECK_EQ(p.invalid, 0);
}

void test_golden_pacer()
{
	golden(44100, fb_44100, sizeof(fb_44100), out_44100, sizeof(out_44100));
	golden(48000, fb_48000, sizeof(fb_48000), out_48000, sizeof(out_48000));
	golden(96000, fb_96000, sizeof(fb_96000), out_96000, sizeof(out_96000));
}
