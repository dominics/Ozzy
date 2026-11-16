/* SPDX-License-Identifier: MIT */
/*
 * The second MIDI port of a Ploytec isoc device (OZZY_QUIRK_MIDI_PORT2: the Dynacord CMS 600-3's
 * "SystemCtrl", common/devices/ploytec/dynacord_midi.h).
 *
 * Its rings are the shared memory's midi2_in and midi2_out, which work like midi_in and
 * midi_out: the engine fills midi2_in and drains midi2_out, a client the other way round
 * (ozzy_midi_ring_push / ozzy_midi_ring_pop). Port 2 goes out in the same MIDI OUT transfers as
 * port 1 (one per millisecond from the bus timer), so it needs no OZZY_METHOD_MIDI_OUT_PENDING.
 * On a device without the quirk the rings stay unused.
 */

#ifndef PLOYTEC_MIDI2_H
#define PLOYTEC_MIDI2_H

#include "ploytec_isoc_engine.h"
#include "../../../common/devices/ploytec/dynacord_midi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Whether @model has a second MIDI port the engine moves */
static inline bool ozzy_device_has_midi2(const struct ozzy_device *model)
{
	return model && model->family == OZZY_FAMILY_PLOYTEC_ISOC && (model->quirks & OZZY_QUIRK_MIDI_PORT2);
}

/* --- The engine's side (ploytec_isoc_engine.c) --- */

/* Port 2's bytes from a MIDI IN transfer of @bytes received at @xfer into its in ring */
static inline void ploytec_midi2_in(volatile struct ozzy_shared_memory *shm, const uint8_t *xfer, uint32_t bytes)
{
	uint8_t out[DYNACORD_MIDI_SLOT_LEN];
	uint32_t count = dynacord_midi_in_port(xfer, bytes, DYNACORD_MIDI_PORT_SYSCTRL, out);

	ozzy_midi_ring_push(&shm->midi2_in, out, count);
}

/*
 * Port 2's slot of the MIDI OUT transfer in @buf (ploytec_midi_xfer_begin has filled it with
 * idle bytes): up to @n bytes from its out ring, the same pace as port 1 (3 or 4)
 */
static inline void ploytec_midi2_out(volatile struct ozzy_shared_memory *shm, uint8_t *buf, uint32_t n)
{
	ozzy_midi_ring_pop(&shm->midi2_out, buf + dynacord_midi_slot(DYNACORD_MIDI_PORT_SYSCTRL),
			   n < DYNACORD_MIDI_SLOT_LEN ? n : DYNACORD_MIDI_SLOT_LEN);
}

#ifdef __cplusplus
}
#endif

#endif /* PLOYTEC_MIDI2_H */
