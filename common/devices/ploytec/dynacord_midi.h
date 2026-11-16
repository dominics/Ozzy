/* SPDX-License-Identifier: MIT */
/*
 * Dynacord CMS 600-3: the second MIDI port in its M4_C1 MIDI frame.
 *
 * The CMS multiplexes two MIDI ports in the 9-byte transfers on its MIDI endpoints (0x04 out,
 * 0x83 in): a 4-byte slot per port, then a control byte. Decoded from USB captures of the
 * Windows driver (dominics/dynacord-cms-pcap):
 *
 *   bytes 0-3  port 1, "USB-MIDI": the DIN sockets (the port ploytec_midi.h moves)
 *   bytes 4-7  port 2, "SystemCtrl": the mixer's FX control
 *   byte  8    0xE0 out, 0xFD in
 *
 * Unused bytes are PLOYTEC_MIDI_IDLE_BYTE (0xFD, an undefined real-time byte, so never MIDI).
 * Out slots are left-aligned, in slots right-aligned; either way the MIDI bytes of a slot are
 * its non-idle bytes, in order. The Windows driver fills port 2's out slot with the same
 * 3-or-4 byte pacing as port 1 (ploytec_midi_xfer_begin).
 *
 * Header only, pure C: the engine (ploytec_isoc_engine.c), the platforms and the host tests
 * all use it without a build change.
 */

#ifndef DYNACORD_MIDI_H
#define DYNACORD_MIDI_H

#include "ploytec_midi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The CMS 600-3's MIDI ports, in their slot order */
enum dynacord_midi_port {
	DYNACORD_MIDI_PORT_USB     = 0,   /* "USB-MIDI": the DIN sockets */
	DYNACORD_MIDI_PORT_SYSCTRL = 1,   /* "SystemCtrl": FX control */
	DYNACORD_MIDI_PORTS        = 2,
};

/* Where things sit in a CMS MIDI transfer */
enum dynacord_midi_layout {
	DYNACORD_MIDI_XFER_LEN    = 9,
	DYNACORD_MIDI_SLOT_LEN    = 4,
	DYNACORD_MIDI_CONTROL     = 8,      /* offset of the control byte */
	DYNACORD_MIDI_OUT_CONTROL = 0xE0,   /* PLOYTEC_MIDI_XFER_CONTROL_IDLE */
	DYNACORD_MIDI_IN_CONTROL  = 0xFD,
};

/* The names the Windows driver gives the ports */
#define DYNACORD_MIDI_PORT_USB_NAME     "USB-MIDI"
#define DYNACORD_MIDI_PORT_SYSCTRL_NAME "SystemCtrl"

/* Offset of @port's slot in a transfer */
static inline uint32_t dynacord_midi_slot(enum dynacord_midi_port port)
{
	return (uint32_t)port * DYNACORD_MIDI_SLOT_LEN;
}

/*
 * dynacord_midi_in_port - @port's MIDI bytes from a MIDI IN transfer of @len bytes received:
 * copies them to @out and returns how many (0-4). Bytes past @len don't count.
 */
static inline uint32_t dynacord_midi_in_port(const uint8_t *xfer, uint32_t len, enum dynacord_midi_port port,
					     uint8_t out[DYNACORD_MIDI_SLOT_LEN])
{
	uint32_t at = dynacord_midi_slot(port), count = 0, i;

	for (i = at; i < at + DYNACORD_MIDI_SLOT_LEN && i < len; i++) {
		out[count] = xfer[i];
		count += xfer[i] != PLOYTEC_MIDI_IDLE_BYTE;
	}
	return count;
}

#ifdef __cplusplus
}
#endif

#endif /* DYNACORD_MIDI_H */
