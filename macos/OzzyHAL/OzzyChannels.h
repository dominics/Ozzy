/* SPDX-License-Identifier: MIT */
#pragma once
#include <cstdint>

/* What a channel carries, for the HAL's preferred channel layout. Discrete
 * channels are labelled by position rather than as a speaker. */
enum class OzzyChannelRole : uint8_t {
	Left,
	Right,
	Discrete,
};

struct OzzyChannelInfo {
	const char* name;
	OzzyChannelRole role;
};

/* CMS 600-3 USB channels, from the mixer's manual. Inputs 1-2 are the master
 * bus (pre master fader, pre GEQ), 3 is the AUX send and 4 the pre-fader MON
 * send. Outputs 1-2 return on stereo strip 5-6 and 3-4 on stereo strip 7-8. */
static constexpr OzzyChannelInfo kOzzyDynacordInputs[] = {
	{ "Master L", OzzyChannelRole::Left },
	{ "Master R", OzzyChannelRole::Right },
	{ "AUX", OzzyChannelRole::Discrete },
	{ "MON", OzzyChannelRole::Discrete },
};

static constexpr OzzyChannelInfo kOzzyDynacordOutputs[] = {
	{ "Ch 5-6 L", OzzyChannelRole::Left },
	{ "Ch 5-6 R", OzzyChannelRole::Right },
	{ "Ch 7-8 L", OzzyChannelRole::Discrete },
	{ "Ch 7-8 R", OzzyChannelRole::Discrete },
};

/* The CMS 600-3 channel at a 1-based element, or nullptr if there is none. */
inline const OzzyChannelInfo* OzzyDynacordChannel(bool output, uint32_t element)
{
	constexpr uint32_t count = sizeof(kOzzyDynacordInputs) / sizeof(kOzzyDynacordInputs[0]);
	static_assert(count == sizeof(kOzzyDynacordOutputs) / sizeof(kOzzyDynacordOutputs[0]));
	if (element < 1 || element > count) return nullptr;
	return output ? &kOzzyDynacordOutputs[element - 1] : &kOzzyDynacordInputs[element - 1];
}

/* The two 1-based channels a stereo client plays to or records from. */
struct OzzyStereoPair {
	uint32_t left = 1;
	uint32_t right = 2;
};

inline bool OzzyStereoPairValid(OzzyStereoPair pair, uint32_t channels)
{
	return pair.left >= 1 && pair.left <= channels && pair.right >= 1 && pair.right <= channels;
}

/* The pair if it fits the device's channel count, otherwise channels 1-2. */
inline OzzyStereoPair OzzyStereoPairFor(OzzyStereoPair pair, uint32_t channels)
{
	return OzzyStereoPairValid(pair, channels) ? pair : OzzyStereoPair {};
}

/* Packs a pair into one word so it can be swapped atomically. */
inline uint32_t OzzyStereoPairPack(OzzyStereoPair pair)
{
	return (pair.left & 0xFFFF) << 16 | (pair.right & 0xFFFF);
}

inline OzzyStereoPair OzzyStereoPairUnpack(uint32_t packed)
{
	return { packed >> 16, packed & 0xFFFF };
}
