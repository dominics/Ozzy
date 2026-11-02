/* SPDX-License-Identifier: MIT */
/*
 * A MIDI byte stream from a device into whole messages, for a client that hands MIDI on as
 * messages rather than bytes (CoreMIDI packets: OzzyMIDI). midi_running_status.h is the other
 * direction (a byte stream to a device).
 *
 * - channel messages, with running status: a data byte with no message open starts a new
 *   message on the last channel status
 * - system common messages (F1-F6) whole; they cancel running status
 * - real-time bytes (F8-FF) on their own, even in the middle of another message, which they
 *   don't disturb
 * - SysEx (F0 ... F7) in chunks of at most MIDI_PARSER_CHUNK bytes, the first starting with F0
 *   and the last ending with F7; a status byte other than F7 or real-time ends an unterminated
 *   one (what came so far goes out as the last chunk)
 * - data bytes with nothing to belong to, and a stray F7, are dropped
 *
 * Pure C with no platform dependencies, usable from C++. Fork only (0.x OzzyMIDI's MidiParser).
 */

#ifndef OZZY_MIDI_PARSER_H
#define OZZY_MIDI_PARSER_H

#if defined(__KERNEL__) && defined(__linux__)
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MIDI_PARSER_CHUNK 256   /* bytes per SysEx chunk at most */

/*
 * struct midi_parser - The parser's state, zero to start (and to reset).
 * @buf:      the message (or SysEx chunk) being built
 * @len:      bytes in @buf
 * @expected: length of the message being built, 0 = none
 * @running:  running status, 0 = none
 * @sysex:    inside a SysEx
 */
struct midi_parser {
	uint8_t buf[MIDI_PARSER_CHUNK];
	uint32_t len;
	uint32_t expected;
	uint8_t running;
	bool sysex;
};

/* Called once per complete message or SysEx chunk */
typedef void (*midi_parser_emit)(const uint8_t *msg, uint32_t len, void *ctx);

/* Data bytes that follow @status */
static inline uint32_t midi_parser_data_len(uint8_t status)
{
	if (status >= 0xF0) {
		switch (status) {
		case 0xF1:
		case 0xF3:
			return 1;
		case 0xF2:
			return 2;
		default:
			return 0;
		}
	}
	return ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) ? 1 : 2;
}

/* SysEx: a data byte or F7 into the chunk being built. False if @b ends the SysEx unterminated. */
static inline bool midi_parser_sysex(struct midi_parser *p, uint8_t b, midi_parser_emit emit, void *ctx)
{
	if (b < 0x80 || b == 0xF7) {
		p->buf[p->len++] = b;
		if (b == 0xF7 || p->len == MIDI_PARSER_CHUNK) {
			emit(p->buf, p->len, ctx);
			p->len = 0;
			p->sysex = b != 0xF7;
		}
		return true;
	}
	if (p->len)
		emit(p->buf, p->len, ctx);
	p->len = 0;
	p->sysex = false;
	return false;
}

/* midi_parser_feed - One byte of the stream; @emit gets each message it completes */
static inline void midi_parser_feed(struct midi_parser *p, uint8_t b, midi_parser_emit emit, void *ctx)
{
	if (b >= 0xF8) {
		emit(&b, 1, ctx);
		return;
	}
	if (p->sysex && midi_parser_sysex(p, b, emit, ctx))
		return;
	if (b == 0xF0) {
		p->sysex = true;
		p->buf[0] = b;
		p->len = 1;
		p->expected = 0;
		p->running = 0;
		return;
	}
	if (b >= 0x80) {
		p->running = b < 0xF0 ? b : 0;
		p->len = 0;
		p->expected = 0;
		if (b == 0xF7)   /* stray end of SysEx */
			return;
		p->buf[0] = b;
		p->len = 1;
		p->expected = 1 + midi_parser_data_len(b);
		if (p->expected == 1) {
			emit(p->buf, 1, ctx);
			p->len = p->expected = 0;
		}
		return;
	}
	if (!p->expected) {   /* a data byte with no message open */
		if (!p->running)
			return;
		p->buf[0] = p->running;
		p->len = 1;
		p->expected = 1 + midi_parser_data_len(p->running);
	}
	p->buf[p->len++] = b;
	if (p->len == p->expected) {
		emit(p->buf, p->len, ctx);
		p->len = p->expected = 0;
	}
}

#ifdef __cplusplus
}
#endif

#endif /* OZZY_MIDI_PARSER_H */
