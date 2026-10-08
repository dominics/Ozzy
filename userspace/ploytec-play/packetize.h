#ifndef PLOYTEC_PLAY_PACKETIZE_H
#define PLOYTEC_PLAY_PACKETIZE_H

#include <stdint.h>
#include "pacer.h"
#include "ring.h"
#include "ploytec_defs.h"

enum fill_mode {
	FILL_NORMAL,     /* audio from the ring; a short ring counts as an underrun */
	FILL_DRAINING,   /* the source has ended: take what the ring has, no underruns */
	FILL_SILENCE,    /* zeros only; the ring is left alone */
};

struct fill_result {
	unsigned long long frames, underruns;
	unsigned long long hist[DYNACORD_OUT_MAX_FRAMES_PER_PKT + 1];
};

/*
 * Fill npkts iso OUT packets back to back into buf, sized by the pacer.
 * Each packet's byte length goes in lengths[]; frames the ring can't supply
 * are zero-filled. Adds to res and returns the total byte count.
 */
int fill_packets(struct pacer *p, struct ring *r, uint8_t *buf, unsigned *lengths, int npkts,
		 enum fill_mode mode, struct fill_result *res);

#endif
