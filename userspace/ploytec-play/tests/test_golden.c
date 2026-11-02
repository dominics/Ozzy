#include <stdlib.h>
#include "test.h"
#include "../pacer.h"
#include "fixtures/counter_44100.h"
#include "fixtures/counter_48000.h"
#include "fixtures/counter_96000.h"

int test_failures;

/*
 * Feed the feedback Windows received through our pacer, in bursts of 5
 * readings every 40 packets as the stream delivers it, and compare the result with the packets Windows sent over the
 * same span: same total frames to within 1 ms, a running difference of at
 * most 3 frames at every point, and per-size packet counts within 1%
 * (minimum tolerance 8).
 */
static void golden(unsigned rate, const unsigned char *fb, size_t nfb,
		   const unsigned char *win, size_t nwin)
{
	struct pacer p;
	long ours[16] = { 0 }, theirs[16] = { 0 }, tot_o = 0, tot_t = 0, drift = 0;
	size_t npk = nfb * 8 < nwin ? nfb * 8 : nwin;

	pacer_init(&p, rate);
	for (size_t i = 0; i < npk; i++) {
		/* The stream receives feedback in transfers of 5 readings. */
		if (i % (8 * 5) == 0)
			for (size_t g = i / 8; g < i / 8 + 5 && g < nfb; g++)
				pacer_feedback(&p, fb[g]);
		int n = pacer_next(&p);
		ours[n]++;
		tot_o += n;
		theirs[win[i]]++;
		tot_t += win[i];
		if (labs(tot_o - tot_t) > drift)
			drift = labs(tot_o - tot_t);
	}
	printf("%u Hz: %zu packets, ours %ld frames, windows %ld frames, max running difference %ld\n",
	       rate, npk, tot_o, tot_t, drift);
	CHECK(labs(tot_o - tot_t) <= (long)(rate / 1000 + 2));
	/* Running difference stays within a few frames, so the device buffer can't drift. */
	CHECK(drift <= 3);
	for (int s = 0; s < 16; s++) {
		long tol = theirs[s] / 100 > 8 ? theirs[s] / 100 : 8;
		if (labs(ours[s] - theirs[s]) > tol)
			printf("  size %d: ours %ld windows %ld\n", s, ours[s], theirs[s]);
		CHECK(labs(ours[s] - theirs[s]) <= tol);
	}
	CHECK_EQ(p.invalid, 0);
}

int main(void)
{
	golden(44100, fb_44100, sizeof(fb_44100), out_44100, sizeof(out_44100));
	golden(48000, fb_48000, sizeof(fb_48000), out_48000, sizeof(out_48000));
	golden(96000, fb_96000, sizeof(fb_96000), out_96000, sizeof(out_96000));
	if (test_failures) {
		fprintf(stderr, "%d failure(s)\n", test_failures);
		return 1;
	}
	printf("golden tests passed\n");
	return 0;
}
