/* SPDX-License-Identifier: MIT */
/* isoc_plan: direct and bounced frame lists, the wrap, IN scatter, the microframe clock */

#include "../isoc_plan.h"
#include "../../../../tests/check.h"

static void fill_ring(uint8_t *ring, uint32_t size)
{
	for (uint32_t i = 0; i < size; i++)
		ring[i] = (uint8_t)(i * 7 + 3);
}

/* Packets of @len bytes back to back from @offset, modulo @size (as the engine's ring mode) */
static void ring_packets(struct ozzy_isoc_packet *p, uint32_t count, uint32_t offset, uint32_t len, uint32_t size)
{
	for (uint32_t i = 0; i < count; i++) {
		p[i] = (struct ozzy_isoc_packet){ (offset + i * len) % size, len, 0, 0, 0 };
	}
}

static void test_direct(void)
{
	uint8_t ring[1200], bounce[256];
	struct ozzy_isoc_packet p[4];
	struct isoc_plan plan;

	fill_ring(ring, sizeof(ring));
	ring_packets(p, 4, 120, 36, sizeof(ring));
	CHECK(isoc_plan_make(&plan, ring, sizeof(ring), p, 4, bounce, sizeof(bounce), true), "direct plan");
	CHECK(!plan.bounced && plan.data == ring + 120 && plan.total == 144, "direct at offset 120, 144 bytes");

	/* Ending exactly at the ring's end doesn't wrap */
	ring_packets(p, 4, 1200 - 144, 36, sizeof(ring));
	CHECK(isoc_plan_make(&plan, ring, sizeof(ring), p, 4, bounce, sizeof(bounce), true) && !plan.bounced,
	      "a transfer ending at the ring's end is direct");
}

static void test_wrap_out(void)
{
	uint8_t ring[1200], bounce[256];
	struct ozzy_isoc_packet p[4];
	struct isoc_plan plan;
	bool same = true;

	fill_ring(ring, sizeof(ring));
	/* 12-byte frames: the second packet straddles the end (1188 + 36 > 1200) */
	ring_packets(p, 4, 1152, 36, sizeof(ring));
	CHECK(isoc_plan_make(&plan, ring, sizeof(ring), p, 4, bounce, sizeof(bounce), true), "wrapping plan");
	CHECK(plan.bounced && plan.data == bounce && plan.total == 144, "a wrapping transfer is bounced");
	for (uint32_t i = 0; i < 144; i++)
		same &= bounce[i] == ring[(1152 + i) % sizeof(ring)];
	CHECK(same, "the bounce holds the ring's bytes in order across the wrap");

	CHECK(!isoc_plan_make(&plan, ring, sizeof(ring), p, 4, bounce, 100, true), "a bounce too small fails");
	CHECK(!isoc_plan_make(&plan, ring, sizeof(ring), p, 4, NULL, 0, true), "no bounce buffer fails");
}

static void test_zero_length(void)
{
	uint8_t ring[1200], bounce[256];
	struct ozzy_isoc_packet p[3] = { { 600, 0, 0, 0, 0 }, { 600, 36, 0, 0, 0 }, { 636, 0, 0, 0, 0 } };
	struct isoc_plan plan;

	CHECK(isoc_plan_make(&plan, ring, sizeof(ring), p, 3, bounce, sizeof(bounce), true), "zero-length packets");
	CHECK(!plan.bounced && plan.data == ring + 600 && plan.total == 36, "empty packets don't break a direct plan");

	struct ozzy_isoc_packet none[2] = { { 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 } };
	CHECK(isoc_plan_make(&plan, ring, sizeof(ring), none, 2, NULL, 0, true) && !plan.bounced && plan.total == 0,
	      "all-empty transfer needs no bounce");
}

static void test_in_scatter(void)
{
	uint8_t buf[512], bounce[512];
	/* Feedback-like: 4-byte slots 64 apart, not back to back, so bounced */
	struct ozzy_isoc_packet p[3] = { { 0, 4, 0, 0, 0 }, { 64, 4, 0, 0, 0 }, { 128, 4, 0, 0, 0 } };
	struct isoc_plan plan;

	memset(buf, 0xEE, sizeof(buf));
	CHECK(isoc_plan_make(&plan, buf, sizeof(buf), p, 3, bounce, sizeof(bounce), false) && plan.bounced,
	      "gapped IN packets are bounced");
	CHECK(plan.total == 12, "IN total is the request lengths");
	for (int i = 0; i < 12; i++)
		bounce[i] = (uint8_t)(0x10 + i);
	p[0].actual = 3;   /* short packet: 3 of 4 bytes */
	p[1].actual = 4;
	p[2].actual = 0;
	isoc_plan_scatter(&plan, buf, sizeof(buf), p, 3);
	CHECK(buf[0] == 0x10 && buf[2] == 0x12 && buf[3] == 0xEE, "short packet scatters its actual bytes only");
	CHECK(buf[64] == 0x14 && buf[67] == 0x17, "second packet from its request position in the bounce");
	CHECK(buf[128] == 0xEE, "empty packet writes nothing");
}

static void test_frame_clock(void)
{
	struct frame_clock fc;
	struct ozzy_isoc_packet p[4];
	const uint64_t hz = 24000000;   /* Apple silicon mach_absolute_time: 3000 ticks a microframe */
	const int64_t true_offset = 5000000;

	frame_clock_init(&fc, hz);
	CHECK(!frame_clock_valid(&fc), "no samples, no clock");
	isoc_plan_times(p, 4, &fc, 100, 8);
	CHECK(p[0].time == 0 && p[3].time == 0, "without a clock the times are 0 (the engine uses its own)");

	/* Readings up to 1 ms late (24000 ticks): the estimate is the earliest */
	static const int64_t late[] = { 20000, 7000, 23999, 1500, 9000, 300, 12000 };
	for (unsigned i = 0; i < sizeof(late) / sizeof(late[0]); i++) {
		uint64_t uframe = 1000 + i * 32;
		frame_clock_sample(&fc, uframe, (uint64_t)(true_offset + (int64_t)uframe * 3000 + late[i]));
	}
	CHECK(frame_clock_valid(&fc), "clock valid after samples");
	CHECK(frame_clock_time(&fc, 2000) == (uint64_t)(true_offset + 2000 * 3000 + 300), "earliest offset wins");

	/* The window forgets: 32 later samples, all 1000 ticks late */
	for (unsigned i = 0; i < FRAME_CLOCK_SAMPLES; i++) {
		uint64_t uframe = 5000 + i * 32;
		frame_clock_sample(&fc, uframe, (uint64_t)(true_offset + (int64_t)uframe * 3000 + 1000));
	}
	CHECK(frame_clock_time(&fc, 9000) == (uint64_t)(true_offset + 9000 * 3000 + 1000), "old samples age out");

	/* Packet times: the end of each packet's interval (8 microframes each) */
	isoc_plan_times(p, 4, &fc, 9000, 8);
	CHECK(p[0].time == frame_clock_time(&fc, 9008) && p[3].time == frame_clock_time(&fc, 9032),
	      "packet k ends at start + (k + 1) * interval");
	CHECK(p[1].time - p[0].time == 8 * 3000, "packets 1 ms apart at 8 microframes");

	/* A 1 GHz host clock (Intel): 125000 ticks a microframe */
	frame_clock_init(&fc, 1000000000);
	frame_clock_sample(&fc, 8000, 1000000000ull + 7);
	CHECK(frame_clock_time(&fc, 8001) == 1000000000ull + 7 + 125000, "1 GHz clock");
}

int main(void)
{
	test_direct();
	test_wrap_out();
	test_zero_length();
	test_in_scatter();
	test_frame_clock();
	return CHECK_DONE("isoc_plan");
}
