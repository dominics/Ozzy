/* SPDX-License-Identifier: MIT */
#include "DynacordRing.h"
#include <string.h>
#include "../../Shared/OzzySharedData.h"

DynacordRingLayout DynacordRingLayoutFor(uint32_t rate)
{
	DynacordRingLayout l;
	const uint64_t bufBytes = (uint64_t)kOzzyMaxPacketSize * kOzzyNumPackets;
	l.outFrames = kDynacordOutRingFrames;
	l.inFramesPerXfer = (rate * 3 / 1000) & ~7u;
	uint64_t slots = bufBytes / ((uint64_t)l.inFramesPerXfer * kDynacordInFrameBytes);
	l.inFrames = (uint32_t)(slots * l.inFramesPerXfer);
	return l;
}

void DynacordTakeOutput(uint8_t* ring, uint32_t ringFrames, uint64_t pos, uint8_t* dst, uint32_t n)
{
	uint32_t start = (uint32_t)(pos % ringFrames);
	uint32_t first = ringFrames - start < n ? ringFrames - start : n;
	uint8_t* a = ring + (size_t)start * kDynacordOutFrameBytes;
	memcpy(dst, a, (size_t)first * kDynacordOutFrameBytes);
	memset(a, 0, (size_t)first * kDynacordOutFrameBytes);
	if (first < n) {
		size_t rest = (size_t)(n - first) * kDynacordOutFrameBytes;
		memcpy(dst + (size_t)first * kDynacordOutFrameBytes, ring, rest);
		memset(ring, 0, rest);
	}
}

uint8_t* DynacordInputSlot(uint8_t* ring, const DynacordRingLayout& layout, uint64_t xferSeq)
{
	uint64_t slots = layout.inFrames / layout.inFramesPerXfer;
	return ring + (size_t)(xferSeq % slots) * layout.inFramesPerXfer * kDynacordInFrameBytes;
}
