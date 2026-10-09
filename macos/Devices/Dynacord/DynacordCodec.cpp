/* SPDX-License-Identifier: MIT */
#include "DynacordCodec.h"
#include <string.h>
#include "../../../common/devices/ploytec/ploytec_codec.h"

static int32_t ToS24(float x)
{
	if (!(x == x)) return 0;   /* NaN: casting it to int is undefined */
	if (x >= 1.0f)
		return 0x7FFFFF;
	if (x <= -1.0f)
		return -0x800000;
	return (int32_t)(x * 8388608.0f);
}

void DynacordEncodeFrame(uint8_t dst[12], const float src[4])
{
	for (int c = 0; c < 4; c++) {
		int32_t s = ToS24(src[c]);
		dst[3 * c] = (uint8_t)(s & 0xFF);
		dst[3 * c + 1] = (uint8_t)((s >> 8) & 0xFF);
		dst[3 * c + 2] = (uint8_t)((s >> 16) & 0xFF);
	}
}

void DynacordWriteOutput(uint8_t* ring, const float* src, uint64_t sampleTime, uint32_t frameCount,
			 uint32_t ringFrames, uint32_t bytesPerFrame)
{
	for (uint32_t i = 0; i < frameCount; i++)
		DynacordEncodeFrame(ring + (size_t)((sampleTime + i) % ringFrames) * bytesPerFrame, src + 4 * i);
}

void DynacordReadInput(float* dst, const uint8_t* ring, uint64_t sampleTime, uint32_t frameCount,
		       uint32_t ringFrames, uint32_t bytesPerFrame)
{
	for (uint32_t i = 0; i < frameCount; i++) {
		uint8_t s24[24];
		ploytec_decode_frame(s24, ring + (size_t)((sampleTime + i) % ringFrames) * bytesPerFrame);
		for (int c = 0; c < 4; c++) {
			int32_t v = s24[3 * c] | (s24[3 * c + 1] << 8) | (s24[3 * c + 2] << 16);
			if (v & 0x800000)
				v -= 0x1000000;
			dst[4 * i + c] = (float)v / 8388608.0f;
		}
	}
}

void DynacordClearOutput(uint8_t* ring, uint32_t bufferSize)
{
	memset(ring, 0, bufferSize);
}
