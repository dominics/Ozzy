#ifndef PLOYTEC_PLAY_PACER_H
#define PLOYTEC_PLAY_PACER_H

/*
 * Decides how many frames go in each 125 us iso OUT packet.
 *
 * Before any feedback, packets follow the nominal rate exactly over time.
 * After that, each group of 8 packets (1 ms) carries one reported per-ms
 * count, spread across the group. Readings arrive in batches, so they are
 * queued and consumed one per group; when the queue is empty the last
 * reading is reused.
 */
#define PACER_QUEUE_LEN 32

struct pacer {
	unsigned rate;
	int nominal_ms;      /* rate / 1000, rounded down */
	unsigned acc;        /* nominal-mode remainder, in units of 1/8000 frame */
	int fb_ms;           /* per-ms count for the current group, 0 before any feedback */
	int queue[PACER_QUEUE_LEN];  /* valid readings not yet used, oldest first */
	int q_head, q_len;
	int group_total;     /* frames for the current 8-packet group */
	int slot;            /* index of the next packet within its group, 0..7 */
	unsigned long long invalid;  /* feedback readings rejected as out of range */
};

void pacer_init(struct pacer *p, unsigned rate);

/* Queue a per-ms count from the feedback endpoint. Values more than 2 away
 * from the nominal rate are counted in p->invalid and ignored. If the queue
 * is full the oldest reading is dropped. */
void pacer_feedback(struct pacer *p, int frames_per_ms);

/* Frames for the next packet. */
int pacer_next(struct pacer *p);

/* Frames for packet `slot` (0..7) of a group that must carry `total` frames.
 * Over slots 0..7 the results sum to `total`, and each is floor(total / 8)
 * or ceil(total / 8). */
int pacer_spread(int total, int slot);

#endif
