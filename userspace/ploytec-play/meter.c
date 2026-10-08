#include <math.h>
#include <stdlib.h>
#include "meter.h"
#include "ploytec_codec.h"

static int frame_aligned(const uint8_t *f)
{
	for (int i = 0; i < 24; i++)
		if ((f[i] & 0xfc) != 0xfc || (f[0x20 + i] & 0xfc) != 0xfc)
			return 0;
	return f[0x1b] == 0xce && f[0x3b] == 0xce;
}

void meter_feed(struct meter *m, const uint8_t *buf, size_t len)
{
	uint8_t s24[24];

	for (; len >= METER_FRAME_SIZE; buf += METER_FRAME_SIZE, len -= METER_FRAME_SIZE) {
		if (!frame_aligned(buf)) {
			m->misaligned++;
			continue;
		}
		ploytec_decode_frame(s24, buf);
		for (int c = 0; c < METER_CHANNELS; c++) {
			int32_t v = s24[3 * c] | s24[3 * c + 1] << 8 | s24[3 * c + 2] << 16;
			v = abs(v & 0x800000 ? v - 0x1000000 : v);
			if (v > m->peak[c])
				m->peak[c] = v;
		}
		m->frames++;
	}
	if (len)
		m->misaligned++;
}

double meter_dbfs(int32_t peak)
{
	return peak > 0 ? 20.0 * log10((double)peak / 8388608.0) : METER_FLOOR_DB;
}
