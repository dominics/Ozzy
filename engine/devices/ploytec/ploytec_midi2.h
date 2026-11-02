/* SPDX-License-Identifier: MIT */
/*
 * The second MIDI port of a Ploytec isoc device (OZZY_QUIRK_MIDI_PORT2: the Dynacord CMS 600-3's
 * "SystemCtrl", common/devices/ploytec/dynacord_midi.h). Fork only.
 *
 * struct ozzy_shared_memory has one MIDI ring pair, so the second port's rings live wherever the
 * platform keeps fork state (macOS daemon: the segment's trailer, OzzyDaemonShared.h; Linux: its
 * chip) and the platform hands them to the engine with ozzy_engine_set_midi2(). Without that
 * call the engine moves port 1 only, byte for byte as upstream's.
 *
 * The rings work like the shared memory's: the engine fills @in and drains @out, a client the
 * other way round (ozzy_midi_ring_push / ozzy_midi_ring_pop). Port 2 goes out in the same MIDI
 * OUT transfers as port 1 (one per millisecond from the bus timer), so it needs no
 * OZZY_METHOD_MIDI_OUT_PENDING.
 */

#ifndef PLOYTEC_MIDI2_H
#define PLOYTEC_MIDI2_H

#include "ploytec_isoc_engine.h"
#include "../../../common/devices/ploytec/dynacord_midi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Whether @model has a second MIDI port the engine can move */
static inline bool ozzy_device_has_midi2(const struct ozzy_device *model)
{
	return model && model->family == OZZY_FAMILY_PLOYTEC_ISOC && (model->quirks & OZZY_QUIRK_MIDI_PORT2);
}

/*
 * ozzy_engine_set_midi2 - The rings for @e's second MIDI port: the engine fills @in and drains
 * @out (NULL, NULL: port 1 only). Call it with the engine stopped (after ozzy_engine_init, before
 * ozzy_engine_start); ozzy_engine_attach keeps it. False, and nothing set, if the model has no
 * second port.
 */
static inline bool ozzy_engine_set_midi2(struct ozzy_engine *e, volatile struct ozzy_midi_buffer *in,
					 volatile struct ozzy_midi_buffer *out)
{
	struct ploytec_isoc_engine *pe = (struct ploytec_isoc_engine *)e;

	if (!e || !ozzy_device_has_midi2(e->model))
		return false;
	pe->midi2_in = in;
	pe->midi2_out = out;
	return true;
}

/* --- The engine's side (ploytec_isoc_engine.c) --- */

/* Port 2's bytes from a MIDI IN transfer of @bytes received at @xfer into its in ring */
static inline void ploytec_midi2_in(struct ploytec_isoc_engine *pe, const uint8_t *xfer, uint32_t bytes)
{
	uint8_t out[DYNACORD_MIDI_SLOT_LEN];
	uint32_t count = dynacord_midi_in_port(xfer, bytes, DYNACORD_MIDI_PORT_SYSCTRL, out);

	ozzy_midi_ring_push(pe->midi2_in, out, count);
}

/*
 * Port 2's slot of the MIDI OUT transfer in @buf (ploytec_midi_xfer_begin has filled it with
 * idle bytes): up to @n bytes from its out ring, the same pace as port 1 (3 or 4)
 */
static inline void ploytec_midi2_out(struct ploytec_isoc_engine *pe, uint8_t *buf, uint32_t n)
{
	ozzy_midi_ring_pop(pe->midi2_out, buf + dynacord_midi_slot(DYNACORD_MIDI_PORT_SYSCTRL),
			   n < DYNACORD_MIDI_SLOT_LEN ? n : DYNACORD_MIDI_SLOT_LEN);
}

#ifdef __cplusplus
}
#endif

#endif /* PLOYTEC_MIDI2_H */
