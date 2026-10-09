/* SPDX-License-Identifier: MIT */
#pragma once
#include <cstdio>
#include <cstdlib>
#include "test.h"

/*
 * Compares our iso OUT packet sizes with the ones the Windows driver sent
 * over the same span: same total frames to within 1 ms, a running
 * difference of at most maxDrift frames at every point, and per-size packet
 * counts within 1% (minimum tolerance 8).
 */
struct GoldenTally {
	static constexpr int kMaxSize = 16;
	long ours[kMaxSize] = {}, theirs[kMaxSize] = {}, totOurs = 0, totTheirs = 0, drift = 0, packets = 0;

	void add(int o, int t)
	{
		CHECK(o >= 0 && o < kMaxSize);
		CHECK(t >= 0 && t < kMaxSize);
		if (o < 0 || o >= kMaxSize || t < 0 || t >= kMaxSize)
			return;
		ours[o]++;
		theirs[t]++;
		totOurs += o;
		totTheirs += t;
		packets++;
		long d = std::labs(totOurs - totTheirs);
		if (d > drift)
			drift = d;
	}

	void check(unsigned rate, long maxDrift) const
	{
		std::printf("%u Hz: %ld packets, ours %ld frames, windows %ld frames, max running difference %ld\n",
			    rate, packets, totOurs, totTheirs, drift);
		CHECK(std::labs(totOurs - totTheirs) <= (long)(rate / 1000 + 2));
		CHECK(drift <= maxDrift);
		for (int s = 0; s < kMaxSize; s++) {
			long tol = theirs[s] / 100 > 8 ? theirs[s] / 100 : 8;
			if (std::labs(ours[s] - theirs[s]) > tol)
				std::printf("  size %d: ours %ld windows %ld\n", s, ours[s], theirs[s]);
			CHECK(std::labs(ours[s] - theirs[s]) <= tol);
		}
	}
};
