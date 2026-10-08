/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

/*
 * Frame-indexed rings shared by OzzyHAL and DynacordEngine. Sample time t
 * lives in slot t % ringFrames. Output frames are S24_3LE x 4 (12 bytes);
 * input frames are raw 64-byte Ploytec capture frames.
 */
enum : uint32_t {
	kDynacordOutFrameBytes = 12,
	kDynacordInFrameBytes = 64,
	kDynacordOutRingFrames = 65536,
};

struct DynacordRingLayout {
	uint32_t outFrames;       /* output ring capacity, frames */
	uint32_t inFrames;        /* input ring capacity: a whole number of transfers */
	uint32_t inFramesPerXfer; /* frames per 3 ms bulk IN transfer */
};

/* Ring sizes for one sample rate. */
DynacordRingLayout DynacordRingLayoutFor(uint32_t rate);

/* Copy n output frames starting at sample position pos into dst, wrapping
 * at ringFrames, and zero the ring frames just read so a stalled or absent
 * writer yields silence instead of a replay. */
void DynacordTakeOutput(uint8_t* ring, uint32_t ringFrames, uint64_t pos, uint8_t* dst, uint32_t n);

/* Start of the input ring slot that bulk IN transfer number xferSeq fills. */
uint8_t* DynacordInputSlot(uint8_t* ring, const DynacordRingLayout& layout, uint64_t xferSeq);
