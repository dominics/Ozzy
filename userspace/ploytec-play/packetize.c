#include <string.h>
#include "packetize.h"

int fill_packets(struct pacer *p, struct ring *r, uint8_t *buf, unsigned *lengths, int npkts,
		 enum fill_mode mode, struct fill_result *res)
{
	int off = 0;

	for (int i = 0; i < npkts; i++) {
		int n = pacer_next(p);
		size_t got = mode == FILL_SILENCE ? 0 : ring_read(r, buf + off, (size_t)n);
		if (got < (size_t)n) {
			memset(buf + off + got * DYNACORD_OUT_FRAME_SIZE, 0,
			       ((size_t)n - got) * DYNACORD_OUT_FRAME_SIZE);
			if (mode == FILL_NORMAL)
				res->underruns++;
		}
		lengths[i] = (unsigned)(n * DYNACORD_OUT_FRAME_SIZE);
		off += n * DYNACORD_OUT_FRAME_SIZE;
		res->frames += (unsigned long long)n;
		res->hist[n]++;
	}
	return off;
}
