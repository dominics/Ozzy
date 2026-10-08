/* SPDX-License-Identifier: MIT */
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../../common/devices/ploytec/ploytec_codec.h"
#include "../Devices/Dynacord/DynacordCodec.h"
#include "../Devices/Dynacord/DynacordRing.h"
#include "test.h"
#include "tests.h"

/* FINDINGS.md walking-bit layout: bit b of channel c sits at byte 3c + b/8,
 * bit b%8. Bit 23 is the sign, reached with -1.0. */
void test_codec_encode_walking_bits()
{
	for (int c = 0; c < 4; c++) {
		for (int b = 0; b < 24; b++) {
			float src[4] = { 0, 0, 0, 0 };
			src[c] = b == 23 ? -1.0f : (float)(1 << b) / 8388608.0f;
			uint8_t dst[12];
			DynacordEncodeFrame(dst, src);
			for (int byte = 0; byte < 12; byte++) {
				uint8_t want = byte == 3 * c + b / 8 ? (uint8_t)(1 << (b % 8)) : 0;
				CHECK_EQ(dst[byte], want);
			}
		}
	}
}

void test_codec_encode_clamps()
{
	float src[4] = { 1.0f, 2.0f, -1.0f, -3.0f };
	uint8_t dst[12];
	DynacordEncodeFrame(dst, src);
	const uint8_t want[12] = { 0xff, 0xff, 0x7f, 0xff, 0xff, 0x7f, 0x00, 0x00, 0x80, 0x00, 0x00, 0x80 };
	CHECK(std::memcmp(dst, want, 12) == 0);
}

void test_codec_read_input_matches_shared_decoder()
{
	const uint32_t ringFrames = 288;
	std::vector<uint8_t> ring(ringFrames * kDynacordInFrameBytes);
	std::srand(1);
	for (uint8_t& b : ring)
		b = (uint8_t)std::rand();
	std::vector<float> got(10 * 4);
	DynacordReadInput(got.data(), ring.data(), ringFrames - 4, 10, ringFrames, kDynacordInFrameBytes);
	for (uint32_t f = 0; f < 10; f++) {
		uint8_t s24[24];
		ploytec_decode_frame(s24, &ring[((ringFrames - 4 + f) % ringFrames) * kDynacordInFrameBytes]);
		for (int c = 0; c < 4; c++) {
			int32_t v = s24[3 * c] | (s24[3 * c + 1] << 8) | (s24[3 * c + 2] << 16);
			if (v & 0x800000)
				v -= 0x1000000;
			CHECK(got[f * 4 + c] == (float)v / 8388608.0f);
		}
	}
}

void test_codec_wrap_large_sample_time()
{
	const uint32_t ringFrames = kDynacordOutRingFrames;
	std::vector<uint8_t> ring((size_t)ringFrames * kDynacordOutFrameBytes, 0);
	float src[3 * 4] = { 0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 0.5f, 0 };
	uint64_t t = (1ull << 41) * ringFrames - 1; /* last slot, then wrap to 0 and 1 */
	DynacordWriteOutput(ring.data(), src, t, 3, ringFrames, kDynacordOutFrameBytes);
	const size_t last = (size_t)(ringFrames - 1) * kDynacordOutFrameBytes;
	CHECK_EQ(ring[last + 2], 0x40);  /* frame 0, channel 1: 0x400000 */
	CHECK_EQ(ring[0 + 5], 0x40);     /* frame 1, channel 2 */
	CHECK_EQ(ring[12 + 8], 0x40);    /* frame 2, channel 3 */
}
