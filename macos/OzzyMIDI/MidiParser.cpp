/* SPDX-License-Identifier: MIT */
#include "MidiParser.h"

/* Data bytes that follow a status byte. */
static size_t DataBytes(uint8_t status)
{
	if (status >= 0xf0) {
		switch (status) {
		case 0xf1:
		case 0xf3:
			return 1;
		case 0xf2:
			return 2;
		default:
			return 0;
		}
	}
	uint8_t hi = status & 0xf0;
	return (hi == 0xc0 || hi == 0xd0) ? 1 : 2;
}

void MidiParserFeed(MidiParser* p, uint8_t b, MidiEmit emit, void* ctx)
{
	if (b >= 0xf8) {   /* real-time: never disturbs the message in progress */
		emit(&b, 1, ctx);
		return;
	}
	if (p->sysex) {
		if (b < 0x80 || b == 0xf7) {
			p->buf[p->len++] = b;
			if (b == 0xf7) {
				emit(p->buf, p->len, ctx);
				p->len = 0;
				p->sysex = false;
			} else if (p->len == MidiParser::kChunk) {
				emit(p->buf, p->len, ctx);
				p->len = 0;
			}
			return;
		}
		/* Any other status byte ends an unterminated SysEx. */
		if (p->len)
			emit(p->buf, p->len, ctx);
		p->len = 0;
		p->sysex = false;
	}
	if (b == 0xf0) {
		p->sysex = true;
		p->buf[0] = b;
		p->len = 1;
		p->expected = 0;
		p->running = 0;
		return;
	}
	if (b >= 0x80) {
		p->running = b < 0xf0 ? b : 0;
		if (b == 0xf7) {   /* stray end of SysEx */
			p->len = p->expected = 0;
			return;
		}
		p->buf[0] = b;
		p->len = 1;
		p->expected = 1 + DataBytes(b);
		if (p->expected == 1) {
			emit(p->buf, 1, ctx);
			p->len = p->expected = 0;
		}
		return;
	}
	if (p->expected == 0) {   /* data byte with no message open */
		if (!p->running)
			return;
		p->buf[0] = p->running;
		p->len = 1;
		p->expected = 1 + DataBytes(p->running);
	}
	p->buf[p->len++] = b;
	if (p->len == p->expected) {
		emit(p->buf, p->len, ctx);
		p->len = p->expected = 0;
	}
}
