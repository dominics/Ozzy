/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>

/*
 * Turns a raw MIDI byte stream into whole messages for CoreMIDI: channel
 * messages (with running status) and system common messages, real-time
 * bytes on their own (even in the middle of another message), and SysEx in
 * chunks of at most kChunk bytes, the first starting with F0 and the last
 * ending with F7. Zero-initialise before use.
 */
struct MidiParser {
	enum : size_t { kChunk = 256 };
	uint8_t buf[kChunk];
	size_t len;
	size_t expected;   /* length of the message being built, 0 = none */
	uint8_t running;   /* running status, 0 = none */
	bool sysex;
};

typedef void (*MidiEmit)(const uint8_t* msg, size_t len, void* ctx);

/* Feed one byte; emit is called once per complete message or SysEx chunk. */
void MidiParserFeed(MidiParser* p, uint8_t byte, MidiEmit emit, void* ctx);
