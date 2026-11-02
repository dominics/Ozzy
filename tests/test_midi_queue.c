/* SPDX-License-Identifier: MIT */
/*
 * ozzy_midi_queue_write, the MIDI out queue in front of the shared ring (OzzyMIDI, a future
 * Windows MIDI client): bytes reach the ring in order across the queue's wrap, what waits goes
 * first, and what doesn't fit the queue either is counted as dropped.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include "engine/shared/ozzy_shared_data.h"
#include "check.h"
#include <string.h>

/*
 * Indices past the ring, as another process sharing the memory could write them: push and
 * pop stay inside the buffer, store masked indices, and pop ends.
 */
static void check_corrupt_indices(void)
{
	static struct {
		struct ozzy_midi_buffer ring;
		uint8_t guard[4096];
	} g;
	static const uint32_t bad[][2] = { { 0xFFFFFFF0u, 5000 }, { 5000, 0xFFFFFFF0u }, { 0x80000400u, 0x400 } };
	uint8_t bytes[2048], out[2048];
	uint32_t i, k, pushed, popped;

	for (i = 0; i < sizeof(bytes); i++)
		bytes[i] = (uint8_t)(i + 1);
	for (k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
		memset(&g, 0xA5, sizeof(g));
		g.ring.write_index = bad[k][0];
		g.ring.read_index = bad[k][1];
		pushed = ozzy_midi_ring_push(&g.ring, bytes, sizeof(bytes));
		CHECK(pushed < OZZY_MIDI_RING_SIZE && g.ring.write_index <= OZZY_MIDI_RING_MASK,
		      "case %u: pushed %u, write index %u", k, pushed, (unsigned int)g.ring.write_index);
		popped = ozzy_midi_ring_pop(&g.ring, out, sizeof(out));
		CHECK(popped < OZZY_MIDI_RING_SIZE && g.ring.read_index <= OZZY_MIDI_RING_MASK,
		      "case %u: popped %u, read index %u", k, popped, (unsigned int)g.ring.read_index);
		for (i = 0; i < sizeof(g.guard) && g.guard[i] == 0xA5; i++)
			;
		CHECK(i == sizeof(g.guard), "case %u: wrote past the ring at byte %u", k, i);
	}
}

int main(void)
{
	static struct ozzy_midi_buffer ring;
	static uint8_t store[256], data[4096], out[8192];
	struct ozzy_midi_queue q = { store, sizeof(store) - 1, 0, 0, 0 };
	uint32_t i, moved, got = 0, sent = 0;

	for (i = 0; i < sizeof(data); i++)
		data[i] = (uint8_t)(i * 7 + 1);

	/* The ring takes 1023 bytes, the queue 256 more; 1500 written: 221 dropped */
	moved = ozzy_midi_queue_write(&ring, &q, data, 1500);
	CHECK(moved == OZZY_MIDI_RING_SIZE - 1, "moved %u", moved);
	CHECK(q.tail - q.head == sizeof(store) && q.dropped == 1500 - moved - sizeof(store), "queued %u, dropped %u",
	      q.tail - q.head, q.dropped);
	sent = moved + sizeof(store);

	/* Drain a bit at a time; 50 new bytes after the first drain must come out after the queued ones */
	while (got < sent + 50) {
		got += ozzy_midi_ring_pop(&ring, out + got, 100);
		ozzy_midi_queue_write(&ring, &q, got == 100 ? data + 2000 : NULL, got == 100 ? 50 : 0);
	}
	CHECK(!ozzy_midi_queue_pending(&q) && q.dropped == 1500 - (OZZY_MIDI_RING_SIZE - 1) - sizeof(store),
	      "%u bytes still queued, %u dropped", q.tail - q.head, q.dropped);
	CHECK(!memcmp(out, data, sent), "queued bytes out of order");
	CHECK(!memcmp(out + sent, data + 2000, 50), "new bytes passed the queued ones");

	/* Empty queue, room in the ring: straight through, nothing queued */
	moved = ozzy_midi_queue_write(&ring, &q, data, 10);
	CHECK(moved == 10 && !ozzy_midi_queue_pending(&q), "direct write moved %u", moved);

	check_corrupt_indices();
	return CHECK_DONE("midi queue");
}
