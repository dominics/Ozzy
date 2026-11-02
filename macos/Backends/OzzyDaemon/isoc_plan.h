/* SPDX-License-Identifier: MIT */
/*
 * IOUSBLib's isochronous calls take one buffer and a frame list, and read (OUT) or write (IN)
 * the frames back to back from it. The engine's packets carry their own offsets into a buffer
 * they may wrap (the HAL's frame ring). isoc_plan maps one onto the other: the engine's buffer
 * itself when the packets lie back to back there without wrapping, else a bounce buffer the
 * packets are gathered into (OUT, at submit) or scattered from (IN, at completion).
 *
 * The packets' completion times come from the bus's microframe clock (frame_clock): IOUSBLib
 * reports none per packet, and GetBusMicroFrameNumber's time is only within 1 ms of the
 * reading, so frame_clock keeps the earliest offset of recent readings.
 *
 * No IOKit in here, so the host tests build it.
 */

#ifndef OZZY_DAEMON_ISOC_PLAN_H
#define OZZY_DAEMON_ISOC_PLAN_H

#include "../../../engine/core/ozzy_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

struct isoc_plan {
	uint8_t *data;      /* the buffer IOUSBLib gets */
	uint32_t total;     /* bytes of all the packets, back to back */
	bool bounced;       /* data is the bounce buffer */
};

/*
 * isoc_plan_make - Plan a transfer of @count @packets over @buf (@buf_size bytes, offsets
 * may wrap). @bounce (@bounce_size bytes) is used when the packets aren't back to back in @buf
 * or wrap; for @out their bytes are gathered into it now. False if a bounce is needed and they
 * don't fit, or a packet is out of @buf.
 */
bool isoc_plan_make(struct isoc_plan *plan, uint8_t *buf, uint32_t buf_size, const struct ozzy_isoc_packet *packets,
		    uint32_t count, uint8_t *bounce, uint32_t bounce_size, bool out);

/* IN completion of a bounced plan: each packet's @actual bytes to its place in @buf */
void isoc_plan_scatter(const struct isoc_plan *plan, uint8_t *buf, uint32_t buf_size,
		       const struct ozzy_isoc_packet *packets, uint32_t count);

#define FRAME_CLOCK_SAMPLES 32

/*
 * struct frame_clock - Host time of the bus's microframes (125 us; full speed reports 1 ms
 * frames, which the caller counts as 8 microframes). Each sample is a microframe number and a
 * host time at or after it began; the earliest host-minus-bus offset of the last
 * FRAME_CLOCK_SAMPLES is the estimate.
 */
struct frame_clock {
	int64_t offset[FRAME_CLOCK_SAMPLES];   /* host ticks minus uframe * ticks_per_uframe */
	uint32_t count;
	uint32_t next;
	int64_t best;
	uint64_t ticks_per_uframe_num;         /* host ticks per microframe = num / den */
	uint64_t ticks_per_uframe_den;
};

/* @host_hz: host clock ticks a second */
void frame_clock_init(struct frame_clock *fc, uint64_t host_hz);
void frame_clock_sample(struct frame_clock *fc, uint64_t uframe, uint64_t host);
bool frame_clock_valid(const struct frame_clock *fc);
/* Host time microframe @uframe begins at */
uint64_t frame_clock_time(const struct frame_clock *fc, uint64_t uframe);

/*
 * isoc_plan_times - Each packet's completion time: the end of its service interval. The
 * transfer starts at microframe @start_uframe, each packet spans @uframes_per_packet.
 */
void isoc_plan_times(struct ozzy_isoc_packet *packets, uint32_t count, const struct frame_clock *fc,
		     uint64_t start_uframe, uint32_t uframes_per_packet);

#ifdef __cplusplus
}
#endif

#endif
