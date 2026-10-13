/* SPDX-License-Identifier: MIT */
#include <cstring>
#include <initializer_list>
#include "../OzzyHAL/OzzyChannels.h"
#include "test.h"
#include "tests.h"

void test_channels_names()
{
	const char* in[] = { "Master L", "Master R", "AUX", "MON" };
	const char* out[] = { "Ch 5-6 L", "Ch 5-6 R", "Ch 7-8 L", "Ch 7-8 R" };
	for (uint32_t e = 1; e <= 4; e++) {
		CHECK(std::strcmp(OzzyDynacordChannel(false, e)->name, in[e - 1]) == 0);
		CHECK(std::strcmp(OzzyDynacordChannel(true, e)->name, out[e - 1]) == 0);
	}
}

void test_channels_out_of_range()
{
	CHECK(OzzyDynacordChannel(false, 0) == nullptr);
	CHECK(OzzyDynacordChannel(true, 0) == nullptr);
	CHECK(OzzyDynacordChannel(false, 5) == nullptr);
	CHECK(OzzyDynacordChannel(true, 5) == nullptr);
	CHECK(OzzyDynacordChannel(true, 0xFFFFFFFF) == nullptr);
}

/* Only the first pair of each scope is a speaker pair; the rest are discrete. */
void test_channels_roles()
{
	for (bool output : { false, true }) {
		CHECK(OzzyDynacordChannel(output, 1)->role == OzzyChannelRole::Left);
		CHECK(OzzyDynacordChannel(output, 2)->role == OzzyChannelRole::Right);
		CHECK(OzzyDynacordChannel(output, 3)->role == OzzyChannelRole::Discrete);
		CHECK(OzzyDynacordChannel(output, 4)->role == OzzyChannelRole::Discrete);
	}
}

void test_channels_stereo_pair()
{
	CHECK(OzzyStereoPairValid({ 1, 2 }, 4));
	CHECK(OzzyStereoPairValid({ 4, 4 }, 4));
	CHECK(OzzyStereoPairValid({ 3, 1 }, 4));
	CHECK(!OzzyStereoPairValid({ 0, 2 }, 4));
	CHECK(!OzzyStereoPairValid({ 1, 5 }, 4));
	CHECK(OzzyStereoPairValid({ 7, 8 }, 8));

	/* A pair stored for the 8-channel device falls back on the 4-channel one. */
	OzzyStereoPair p = OzzyStereoPairFor({ 7, 8 }, 4);
	CHECK_EQ(p.left, 1);
	CHECK_EQ(p.right, 2);
	p = OzzyStereoPairFor({ 3, 4 }, 4);
	CHECK_EQ(p.left, 3);
	CHECK_EQ(p.right, 4);

	p = OzzyStereoPairUnpack(OzzyStereoPairPack({ 4, 3 }));
	CHECK_EQ(p.left, 4);
	CHECK_EQ(p.right, 3);
}
