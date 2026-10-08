/* SPDX-License-Identifier: MIT */
#ifdef __KERNEL__
#include <linux/kernel.h>
#else
#include <stdlib.h>
#endif
#include "dynacord_pacer.h"
#include "ploytec_defs.h"

void pacer_init(struct pacer *p, unsigned rate)
{
	p->rate = rate;
	p->nominal_ms = (int)(rate / 1000);
	p->acc = 0;
	p->fb_ms = 0;
	p->q_head = 0;
	p->q_len = 0;
	p->group_total = 0;
	p->slot = 0;
	p->invalid = 0;
}

void pacer_feedback(struct pacer *p, int frames_per_ms)
{
	if (frames_per_ms < 0 || abs(frames_per_ms - p->nominal_ms) > 2) {
		p->invalid++;
		return;
	}
	if (p->q_len == PACER_QUEUE_LEN) {
		p->q_head = (p->q_head + 1) % PACER_QUEUE_LEN;
		p->q_len--;
	}
	p->queue[(p->q_head + p->q_len) % PACER_QUEUE_LEN] = frames_per_ms;
	p->q_len++;
}

int pacer_spread(int total, int slot)
{
	/* Cumulative frames due after slot+1 packets minus those due after slot,
	 * so extra frames are spaced evenly across the group. */
	return (total * (slot + 1)) / DYNACORD_PKTS_PER_MS - (total * slot) / DYNACORD_PKTS_PER_MS;
}

int pacer_next(struct pacer *p)
{
	int n;

	if (p->slot == 0) {
		if (p->q_len > 0) {
			p->fb_ms = p->queue[p->q_head];
			p->q_head = (p->q_head + 1) % PACER_QUEUE_LEN;
			p->q_len--;
		}
		p->group_total = p->fb_ms;
	}

	if (p->group_total == 0) {
		p->acc += p->rate;
		n = (int)(p->acc / (1000 * DYNACORD_PKTS_PER_MS));
		p->acc -= (unsigned)n * 1000 * DYNACORD_PKTS_PER_MS;
	} else {
		n = pacer_spread(p->group_total, p->slot);
	}

	p->slot = (p->slot + 1) % DYNACORD_PKTS_PER_MS;
	if (n > DYNACORD_OUT_MAX_FRAMES_PER_PKT)
		n = DYNACORD_OUT_MAX_FRAMES_PER_PKT;
	return n;
}
