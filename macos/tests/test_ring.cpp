/* SPDX-License-Identifier: MIT */
#include <cstring>
#include <vector>
#include "../Devices/Dynacord/DynacordRing.h"
#include "../Shared/OzzySharedData.h"
#include "test.h"
#include "tests.h"

static_assert(kDynacordOutRingFrames * kDynacordOutFrameBytes <= kOzzyMaxPacketSize * kOzzyNumPackets,
	      "output ring must fit OzzyAudioBuffer::outputBuffer");

void test_ring_layout()
{
	DynacordRingLayout a = DynacordRingLayoutFor(44100), b = DynacordRingLayoutFor(48000),
			   c = DynacordRingLayoutFor(96000);
	CHECK_EQ(a.inFramesPerXfer, 128);
	CHECK_EQ(b.inFramesPerXfer, 144);
	CHECK_EQ(c.inFramesPerXfer, 288);
	for (const DynacordRingLayout& l : { a, b, c }) {
		CHECK_EQ(l.outFrames, kDynacordOutRingFrames);
		CHECK_EQ(l.inFrames % l.inFramesPerXfer, 0);
		CHECK((uint64_t)l.inFrames * kDynacordInFrameBytes <= kOzzyMaxPacketSize * kOzzyNumPackets);
		/* The largest whole number of transfers that fits. */
		CHECK((uint64_t)(l.inFrames + l.inFramesPerXfer) * kDynacordInFrameBytes >
		      kOzzyMaxPacketSize * kOzzyNumPackets);
	}
}

static std::vector<uint8_t> PatternRing(uint32_t frames)
{
	std::vector<uint8_t> ring(frames * kDynacordOutFrameBytes);
	for (size_t i = 0; i < ring.size(); i++)
		ring[i] = (uint8_t)(i * 7 + 1);
	return ring;
}

static void CheckTake(uint32_t ringFrames, uint64_t pos, uint32_t n)
{
	std::vector<uint8_t> ring = PatternRing(ringFrames), orig = ring;
	std::vector<uint8_t> dst(n * kDynacordOutFrameBytes, 0xAA);
	DynacordTakeOutput(ring.data(), ringFrames, pos, dst.data(), n);
	for (uint32_t f = 0; f < n; f++) {
		size_t slot = (size_t)((pos + f) % ringFrames) * kDynacordOutFrameBytes;
		CHECK(std::memcmp(&dst[f * kDynacordOutFrameBytes], &orig[slot], kDynacordOutFrameBytes) == 0);
		for (uint32_t b = 0; b < kDynacordOutFrameBytes; b++)
			CHECK_EQ(ring[slot + b], 0);
	}
}

void test_ring_take_output_wraps_and_zeroes()
{
	CheckTake(64, 0, 13);
	CheckTake(64, 58, 13);   /* 6 frames before the wrap, 7 after */
	CheckTake(64, 63, 1);
}

void test_ring_take_output_large_position()
{
	CheckTake(kDynacordOutRingFrames, (1ull << 40) + 3, 13);
	CheckTake(kDynacordOutRingFrames, (uint64_t)kDynacordOutRingFrames * 70000 - 5, 13);
}

void test_ring_input_slot()
{
	DynacordRingLayout l = DynacordRingLayoutFor(48000);
	std::vector<uint8_t> ring((size_t)l.inFrames * kDynacordInFrameBytes);
	uint32_t slots = l.inFrames / l.inFramesPerXfer;
	CHECK(DynacordInputSlot(ring.data(), l, 0) == ring.data());
	CHECK(DynacordInputSlot(ring.data(), l, 1) == ring.data() + l.inFramesPerXfer * kDynacordInFrameBytes);
	CHECK(DynacordInputSlot(ring.data(), l, slots) == ring.data());
	CHECK(DynacordInputSlot(ring.data(), l, (1ull << 40) * slots + 2) ==
	      ring.data() + 2 * l.inFramesPerXfer * kDynacordInFrameBytes);
}
