/* SPDX-License-Identifier: MIT */
#include <vector>
#include "../OzzyMIDI/MidiParser.h"
#include "test.h"
#include "tests.h"

typedef std::vector<std::vector<uint8_t>> Msgs;

static void Collect(const uint8_t* m, size_t n, void* ctx)
{
	((Msgs*)ctx)->push_back(std::vector<uint8_t>(m, m + n));
}

static Msgs Run(std::initializer_list<uint8_t> bytes)
{
	MidiParser p = {};
	Msgs out;
	for (uint8_t b : bytes)
		MidiParserFeed(&p, b, Collect, &out);
	return out;
}

void test_parser_channel_messages()
{
	CHECK(Run({ 0x90, 0x3c, 0x64, 0xc0, 0x05 }) == Msgs({ { 0x90, 0x3c, 0x64 }, { 0xc0, 0x05 } }));
}

void test_parser_running_status()
{
	CHECK(Run({ 0x90, 0x3c, 0x64, 0x3e, 0x64, 0x40, 0x00 }) ==
	      Msgs({ { 0x90, 0x3c, 0x64 }, { 0x90, 0x3e, 0x64 }, { 0x90, 0x40, 0x00 } }));
}

void test_parser_realtime_inside_message()
{
	CHECK(Run({ 0x90, 0x3c, 0xf8, 0x64 }) == Msgs({ { 0xf8 }, { 0x90, 0x3c, 0x64 } }));
}

void test_parser_system_common_clears_running_status()
{
	CHECK(Run({ 0x90, 0x3c, 0x64, 0xf6, 0x3c, 0x64 }) == Msgs({ { 0x90, 0x3c, 0x64 }, { 0xf6 } }));
}

void test_parser_stray_data_dropped()
{
	CHECK(Run({ 0x3c, 0x64 }).empty());
}

void test_parser_sysex_short()
{
	CHECK(Run({ 0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7 }) == Msgs({ { 0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7 } }));
}

void test_parser_sysex_long_chunks()
{
	MidiParser p = {};
	Msgs out;
	MidiParserFeed(&p, 0xf0, Collect, &out);
	for (int i = 0; i < 300; i++)
		MidiParserFeed(&p, (uint8_t)(i & 0x7f), Collect, &out);
	MidiParserFeed(&p, 0xf7, Collect, &out);
	CHECK_EQ(out.size(), 2);
	CHECK_EQ(out[0].size(), MidiParser::kChunk);
	CHECK_EQ(out[0][0], 0xf0);
	CHECK_EQ(out[1].size(), 301 + 1 - MidiParser::kChunk);
	CHECK_EQ(out[1].back(), 0xf7);
}

void test_parser_unterminated_sysex_flushed_by_status()
{
	CHECK(Run({ 0xf0, 0x01, 0x02, 0x90, 0x3c, 0x64 }) == Msgs({ { 0xf0, 0x01, 0x02 }, { 0x90, 0x3c, 0x64 } }));
}
