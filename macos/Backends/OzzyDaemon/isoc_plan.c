/* SPDX-License-Identifier: MIT */
/* Isochronous transfers onto IOUSBLib's back-to-back frame lists (isoc_plan.h) */

#include "isoc_plan.h"

/* @len bytes at @offset of the ring @buf (@size bytes) to @dst, wrapping at its end */
static void ring_read(uint8_t *dst, const uint8_t *buf, uint32_t size, uint32_t offset, uint32_t len)
{
	uint32_t first = size - offset < len ? size - offset : len;

	memcpy(dst, buf + offset, first);
	if (len > first)
		memcpy(dst + first, buf, len - first);
}

static void ring_write(uint8_t *buf, uint32_t size, uint32_t offset, const uint8_t *src, uint32_t len)
{
	uint32_t first = size - offset < len ? size - offset : len;

	memcpy(buf + offset, src, first);
	if (len > first)
		memcpy(buf, src + first, len - first);
}

bool isoc_plan_make(struct isoc_plan *plan, uint8_t *buf, uint32_t buf_size, const struct ozzy_isoc_packet *packets,
		    uint32_t count, uint8_t *bounce, uint32_t bounce_size, bool out)
{
	uint64_t total = 0;
	bool direct = count > 0;
	uint32_t p, pos;

	for (p = 0; p < count; p++) {
		if (packets[p].offset >= buf_size && packets[p].length)
			return false;
		if (packets[p].length > buf_size)
			return false;
		if (packets[p].length && packets[0].offset + total != packets[p].offset)
			direct = false;
		total += packets[p].length;
	}
	plan->total = (uint32_t)total;
	if (count && packets[0].offset + total > buf_size)
		direct = false;   /* wraps at the end of buf */
	if (direct || total == 0) {
		plan->data = buf + (count && packets[0].offset < buf_size ? packets[0].offset : 0);
		plan->bounced = false;
		return true;
	}
	if (!bounce || total > bounce_size)
		return false;
	plan->data = bounce;
	plan->bounced = true;
	if (out) {
		for (p = 0, pos = 0; p < count; pos += packets[p].length, p++)
			ring_read(bounce + pos, buf, buf_size, packets[p].offset, packets[p].length);
	}
	return true;
}

void isoc_plan_scatter(const struct isoc_plan *plan, uint8_t *buf, uint32_t buf_size,
		       const struct ozzy_isoc_packet *packets, uint32_t count)
{
	uint32_t p, pos;

	if (!plan->bounced)
		return;
	for (p = 0, pos = 0; p < count; pos += packets[p].length, p++) {
		uint32_t n = packets[p].actual < packets[p].length ? packets[p].actual : packets[p].length;

		if (n)
			ring_write(buf, buf_size, packets[p].offset, plan->data + pos, n);
	}
}

void frame_clock_init(struct frame_clock *fc, uint64_t host_hz)
{
	memset(fc, 0, sizeof(*fc));
	/* 125 us = host_hz / 8000 ticks */
	fc->ticks_per_uframe_num = host_hz;
	fc->ticks_per_uframe_den = 8000;
}

static int64_t uframe_ticks(const struct frame_clock *fc, uint64_t uframe)
{
	/* uframe * num / den without overflowing: whole seconds first */
	uint64_t secs = uframe / 8000, rest = uframe % 8000;

	return (int64_t)(secs * fc->ticks_per_uframe_num + rest * fc->ticks_per_uframe_num / fc->ticks_per_uframe_den);
}

void frame_clock_sample(struct frame_clock *fc, uint64_t uframe, uint64_t host)
{
	uint32_t i;

	fc->offset[fc->next] = (int64_t)host - uframe_ticks(fc, uframe);
	fc->next = (fc->next + 1) % FRAME_CLOCK_SAMPLES;
	if (fc->count < FRAME_CLOCK_SAMPLES)
		fc->count++;
	fc->best = fc->offset[(fc->next + FRAME_CLOCK_SAMPLES - 1) % FRAME_CLOCK_SAMPLES];
	for (i = 0; i < fc->count; i++) {
		if (fc->offset[i] < fc->best)
			fc->best = fc->offset[i];
	}
}

bool frame_clock_valid(const struct frame_clock *fc)
{
	return fc->count > 0;
}

uint64_t frame_clock_time(const struct frame_clock *fc, uint64_t uframe)
{
	return (uint64_t)(fc->best + uframe_ticks(fc, uframe));
}

void isoc_plan_times(struct ozzy_isoc_packet *packets, uint32_t count, const struct frame_clock *fc,
		     uint64_t start_uframe, uint32_t uframes_per_packet)
{
	uint32_t p;

	for (p = 0; p < count; p++)
		packets[p].time = frame_clock_valid(fc)
			? frame_clock_time(fc, start_uframe + (uint64_t)(p + 1) * uframes_per_packet)
			: 0;
}
