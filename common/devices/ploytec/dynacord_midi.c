/* SPDX-License-Identifier: MIT */
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#include "dynacord_midi.h"

void dynacord_midi_out_init(uint8_t *pkt)
{
	memset(pkt, DYNACORD_MIDI_FILL, DYNACORD_MIDI_TRAILER);
	pkt[DYNACORD_MIDI_TRAILER] = DYNACORD_MIDI_OUT_TRAILER;
}

unsigned int dynacord_midi_in_slot(const uint8_t *pkt, size_t len,
				   enum dynacord_midi_port port,
				   const uint8_t **data)
{
	const uint8_t *slot = pkt + port * DYNACORD_MIDI_SLOT_LEN;
	unsigned int first = 0, end = DYNACORD_MIDI_SLOT_LEN;

	*data = slot;
	if (len < DYNACORD_MIDI_PKT_LEN)
		return 0;

	/* Data is one run at the right; trim fill from both ends regardless */
	while (first < end && slot[first] == DYNACORD_MIDI_FILL)
		first++;
	while (end > first && slot[end - 1] == DYNACORD_MIDI_FILL)
		end--;

	*data = slot + first;
	return end - first;
}
