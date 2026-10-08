#ifndef PLOYTEC_PLAY_STREAM_H
#define PLOYTEC_PLAY_STREAM_H

#include <stddef.h>
#include "device.h"
#include "meter.h"
#include "ring.h"
#include "ploytec_defs.h"

struct stats {
	unsigned long long frames_sent, fb_sum, fb_count, underruns, iso_errors, in_bytes;
	unsigned long long pkt_hist[DYNACORD_OUT_MAX_FRAMES_PER_PKT + 1];
	unsigned long long fb_invalid;
	struct meter in;   /* peaks since the previous stream_stats call */
};

struct stream;

#define STREAM_MAX_OUT_XFERS 64

/*
 * Start streaming: iso OUT audio from the ring, iso IN feedback driving the
 * pacer, and bulk IN reads that feed the input meter. out_xfers iso OUT
 * transfers of 3 ms each are kept queued (1..STREAM_MAX_OUT_XFERS). Returns NULL
 * with a message in err on failure.
 */
struct stream *stream_start(struct device *d, struct ring *ring, unsigned rate, int out_xfers, char *err,
			    size_t errlen);

/* Send a little silence, cancel all transfers, wait for them, and free s. */
void stream_stop(struct stream *s);

/* The source has ended: send what is left in the ring without counting underruns. */
void stream_set_draining(struct stream *s);

/* Copy the stats out and reset the input peaks. */
void stream_stats(struct stream *s, struct stats *out);

/* 1 once the device has gone away or a transfer could not be resubmitted. */
int stream_failed(struct stream *s);

#endif
