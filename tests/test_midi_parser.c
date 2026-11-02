/* SPDX-License-Identifier: MIT */
/*
 * The MIDI input parser (common/midi_parser.h): whole messages out of a device's byte stream,
 * with running status, real-time bytes inside other messages, and SysEx in chunks.
 */

#include "check.h"
#include "common/midi_parser.h"
#include <string.h>

/* What the parser emitted: the messages end to end, each one's length */
struct out {
	uint8_t bytes[4096];
	uint32_t len;
	uint32_t msg_len[512];
	uint32_t msgs;
};

static void collect(const uint8_t *msg, uint32_t len, void *ctx)
{
	struct out *o = ctx;

	memcpy(o->bytes + o->len, msg, len);
	o->len += len;
	o->msg_len[o->msgs++] = len;
}

static void run(struct out *o, const uint8_t *in, uint32_t n)
{
	struct midi_parser p;
	uint32_t i;

	memset(&p, 0, sizeof(p));
	memset(o, 0, sizeof(*o));
	for (i = 0; i < n; i++)
		midi_parser_feed(&p, in[i], collect, o);
}

/* @in gives @msgs messages that are @expect end to end, the k-th @lens[k] bytes long */
static void expect(const char *what, const uint8_t *in, uint32_t n, const uint8_t *exp, uint32_t exp_len,
		   const uint32_t *lens, uint32_t msgs)
{
	struct out o;

	run(&o, in, n);
	CHECK(o.msgs == msgs && o.len == exp_len && !memcmp(o.bytes, exp, exp_len) &&
	      !memcmp(o.msg_len, lens, msgs * sizeof(*lens)), "%s: %u messages, %u bytes", what, o.msgs, o.len);
}

#define EXPECT(what, in, exp, ...) do { \
	static const uint8_t in_[] = in, exp_[] = exp; \
	static const uint32_t lens_[] = __VA_ARGS__; \
	expect(what, in_, sizeof(in_), exp_, sizeof(exp_), lens_, sizeof(lens_) / sizeof(lens_[0])); \
} while (0)
#define B(...) { __VA_ARGS__ }

int main(void)
{
	struct midi_parser p;
	struct out o;
	uint32_t i;

	EXPECT("channel messages", B(0x90, 0x3C, 0x64, 0xC0, 0x05), B(0x90, 0x3C, 0x64, 0xC0, 0x05), { 3, 2 });
	EXPECT("running status", B(0x90, 0x3C, 0x64, 0x3E, 0x64, 0x40, 0x00),
	       B(0x90, 0x3C, 0x64, 0x90, 0x3E, 0x64, 0x90, 0x40, 0x00), { 3, 3, 3 });
	EXPECT("running status, one data byte", B(0xD0, 0x01, 0x03, 0x04),
	       B(0xD0, 0x01, 0xD0, 0x03, 0xD0, 0x04), { 2, 2, 2 });
	EXPECT("real-time inside a message", B(0x90, 0x3C, 0xF8, 0x64), B(0xF8, 0x90, 0x3C, 0x64), { 1, 3 });
	EXPECT("system common cancels running status", B(0x90, 0x3C, 0x64, 0xF6, 0x3C, 0x64),
	       B(0x90, 0x3C, 0x64, 0xF6), { 3, 1 });
	EXPECT("song position", B(0xF2, 0x10, 0x20, 0xF3, 0x05), B(0xF2, 0x10, 0x20, 0xF3, 0x05), { 3, 2 });
	EXPECT("SysEx", B(0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7), B(0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7), { 6 });
	EXPECT("real-time inside SysEx", B(0xF0, 0x01, 0xF8, 0x02, 0xF7), B(0xF8, 0xF0, 0x01, 0x02, 0xF7), { 1, 4 });
	EXPECT("unterminated SysEx ended by a status", B(0xF0, 0x01, 0x02, 0x90, 0x3C, 0x64),
	       B(0xF0, 0x01, 0x02, 0x90, 0x3C, 0x64), { 3, 3 });

	run(&o, (const uint8_t[]){ 0x3C, 0x64, 0xF7, 0x10 }, 4);
	CHECK(o.msgs == 0, "stray data and F7: %u messages", o.msgs);

	/* A SysEx longer than a chunk: F0, 300 data bytes, F7 */
	memset(&p, 0, sizeof(p));
	memset(&o, 0, sizeof(o));
	midi_parser_feed(&p, 0xF0, collect, &o);
	for (i = 0; i < 300; i++)
		midi_parser_feed(&p, (uint8_t)(i & 0x7F), collect, &o);
	midi_parser_feed(&p, 0xF7, collect, &o);
	CHECK(o.msgs == 2 && o.msg_len[0] == MIDI_PARSER_CHUNK && o.msg_len[1] == 302 - MIDI_PARSER_CHUNK &&
	      o.bytes[0] == 0xF0 && o.bytes[o.len - 1] == 0xF7 && o.bytes[1] == 0 && o.bytes[300] == (299 & 0x7F),
	      "long SysEx: %u chunks", o.msgs);
	return CHECK_DONE("midi parser");
}
