/* SPDX-License-Identifier: MIT */
#ifndef DYNACORD_MIDI_H
#define DYNACORD_MIDI_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

/*
 * CMS 600-3 MIDI on bulk 0x04 (out) and 0x83 (in). Every transfer is one
 * 9-byte packet: a 4-byte slot per port, then a trailer byte. Each slot
 * carries the next 0-4 bytes of that port's MIDI byte stream; unused bytes
 * are DYNACORD_MIDI_FILL. Out slots are left-aligned, in slots right-aligned.
 */
enum dynacord_midi_port {
	DYNACORD_MIDI_PORT_USB     = 0,  /* "DYNACORD USB-MIDI": DIN MIDI */
	DYNACORD_MIDI_PORT_SYSCTRL = 1,  /* "DYNACORD SystemCtrl": FX control */
	DYNACORD_MIDI_PORTS        = 2,
};

enum dynacord_midi_layout {
	DYNACORD_MIDI_PKT_LEN     = 9,
	DYNACORD_MIDI_SLOT_LEN    = 4,
	DYNACORD_MIDI_TRAILER     = 8,     /* offset of the trailer byte */
	DYNACORD_MIDI_FILL        = 0xfd,  /* undefined MIDI real-time byte */
	DYNACORD_MIDI_OUT_TRAILER = 0xe0,
	DYNACORD_MIDI_IN_TRAILER  = 0xfd,
};

/*
 * Reset an out packet to empty slots and the out trailer. Callers then
 * write up to DYNACORD_MIDI_SLOT_LEN bytes at dynacord_midi_slot().
 */
void dynacord_midi_out_init(uint8_t *pkt);

/* Start of @port's slot in a packet. */
static inline uint8_t *dynacord_midi_slot(uint8_t *pkt, enum dynacord_midi_port port)
{
	return pkt + port * DYNACORD_MIDI_SLOT_LEN;
}

/*
 * Find @port's MIDI bytes in an in packet of @len bytes. Sets *@data to
 * the first byte and returns the count (0-4); 0 for an empty slot or a
 * short packet.
 */
unsigned int dynacord_midi_in_slot(const uint8_t *pkt, size_t len,
				   enum dynacord_midi_port port,
				   const uint8_t **data);

#endif
