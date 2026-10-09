/* SPDX-License-Identifier: MIT */
#include <initializer_list>
#include <vector>
#include "../../common/devices/ploytec/dynacord_midi.h"
#include "engine_fixture.h"
#include "test.h"
#include "tests.h"

static void Put(OzzyMidiBuffer& ring, std::initializer_list<uint8_t> bytes)
{
	uint32_t w = ring.writeIndex;
	for (uint8_t b : bytes) {
		ring.buffer[w] = b;
		w = (w + 1) & kOzzyMidiRingMask;
	}
	ring.writeIndex = w;
}

static std::vector<uint8_t> Take(OzzyMidiBuffer& ring)
{
	std::vector<uint8_t> v;
	uint32_t r = ring.readIndex;
	while (r != ring.writeIndex) {
		v.push_back(ring.buffer[r]);
		r = (r + 1) & kOzzyMidiRingMask;
	}
	ring.readIndex = r;
	return v;
}

/* Complete the oldest iso OUT transfer, which is when the engine pumps MIDI. */
static void TickOut(EngineFixture& f)
{
	FakeBus::Iso s = f.bus.PopIso(DYNACORD_EP_PCM_OUT);
	for (uint32_t i = 0; i < s.frameCount; i++)
		s.frames[i] = { s.frames[i].reqCount, s.frames[i].reqCount, 0 };
	f.engine.OnIsochComplete(DYNACORD_EP_PCM_OUT, s.ctx, 0, 0, s.frames, s.frameCount);
}

static size_t Pending(EngineFixture& f, uint8_t pipe)
{
	size_t n = 0;
	for (const auto& b : f.bus.bulk)
		n += b.pipe == pipe;
	return n;
}

void test_engine_midi_publishes_ports_and_queues_reads()
{
	EngineFixture f;
	CHECK(f.engine.Start());
	CHECK_EQ(f.shm->midiPortCount, DYNACORD_MIDI_PORTS);
	CHECK_EQ(Pending(f, DYNACORD_EP_MIDI_IN), DynacordEngine::kMidiInXfers);
	for (const auto& b : f.bus.bulk)
		if (b.pipe == DYNACORD_EP_MIDI_IN)
			CHECK_EQ(b.size, DynacordEngine::kMidiInXferBytes);
	CHECK_EQ(Pending(f, DYNACORD_EP_MIDI_OUT), 0);
}

void test_engine_midi_out_packs_both_ports()
{
	EngineFixture f;
	CHECK(f.engine.Start());
	Put(f.shm->midiOut, { 0x90, 0x3c, 0x64, 0x80, 0x3c, 0x00 });
	Put(f.shm->midiOutPort1, { 0x92, 0x30, 0x7f });
	TickOut(f);
	FakeBus::Bulk b = f.bus.PopBulk(DYNACORD_EP_MIDI_OUT);
	CHECK_EQ(b.size, DYNACORD_MIDI_PKT_LEN);
	const uint8_t want[9] = { 0x90, 0x3c, 0x64, 0x80, 0x92, 0x30, 0x7f, 0xfd, 0xe0 };
	CHECK(std::memcmp(b.buffer, want, 9) == 0);
	CHECK_EQ(f.shm->midiOut.readIndex, 4);
	CHECK_EQ(f.shm->midiOutPort1.readIndex, 3);

	/* One packet in flight: another tick sends nothing until it completes. */
	TickOut(f);
	CHECK_EQ(Pending(f, DYNACORD_EP_MIDI_OUT), 0);
	f.engine.OnPacketComplete(DYNACORD_EP_MIDI_OUT, b.ctx, 0, 9);
	FakeBus::Bulk b2 = f.bus.PopBulk(DYNACORD_EP_MIDI_OUT);
	const uint8_t want2[9] = { 0x3c, 0x00, 0xfd, 0xfd, 0xfd, 0xfd, 0xfd, 0xfd, 0xe0 };
	CHECK(std::memcmp(b2.buffer, want2, 9) == 0);
	CHECK_EQ(f.engine.MidiOutPackets(), 2);
}

void test_engine_midi_in_routes_ports()
{
	EngineFixture f;
	CHECK(f.engine.Start());
	FakeBus::Bulk b = f.bus.PopBulk(DYNACORD_EP_MIDI_IN);
	/* Two packets: in slots are right-aligned, trailer 0xfd. */
	const uint8_t pkts[18] = {
		0xfd, 0x90, 0x3c, 0x64,  0xfd, 0xfd, 0xfd, 0xfd,  0xfd,
		0xfd, 0xfd, 0xfd, 0xf8,  0xfd, 0xb0, 0x07, 0x40,  0xfd,
	};
	std::memcpy(b.buffer, pkts, sizeof(pkts));
	f.engine.OnPacketComplete(DYNACORD_EP_MIDI_IN, b.ctx, 0, sizeof(pkts));
	CHECK(Take(f.shm->midiIn) == std::vector<uint8_t>({ 0x90, 0x3c, 0x64, 0xf8 }));
	CHECK(Take(f.shm->midiInPort1) == std::vector<uint8_t>({ 0xb0, 0x07, 0x40 }));
	CHECK_EQ(Pending(f, DYNACORD_EP_MIDI_IN), DynacordEngine::kMidiInXfers);   /* resubmitted */
}

void test_engine_midi_in_overflow_counts_drops()
{
	EngineFixture f;
	CHECK(f.engine.Start());
	f.shm->midiIn.readIndex = 0;
	f.shm->midiIn.writeIndex = kOzzyMidiRingSize - 2;   /* room for exactly 1 byte */
	FakeBus::Bulk b = f.bus.PopBulk(DYNACORD_EP_MIDI_IN);
	const uint8_t pkt[9] = { 0xfd, 0x90, 0x3c, 0x64, 0xfd, 0xfd, 0xfd, 0xfd, 0xfd };
	std::memcpy(b.buffer, pkt, 9);
	f.engine.OnPacketComplete(DYNACORD_EP_MIDI_IN, b.ctx, 0, 9);
	CHECK_EQ(f.shm->midiIn.writeIndex, kOzzyMidiRingSize - 1);
	CHECK_EQ(f.engine.MidiInDropped(), 2);
}

void test_engine_midi_stopped_does_not_resubmit()
{
	EngineFixture f;
	CHECK(f.engine.Start());
	f.engine.Stop();
	size_t before = Pending(f, DYNACORD_EP_MIDI_IN);
	FakeBus::Bulk b = f.bus.PopBulk(DYNACORD_EP_MIDI_IN);
	f.engine.OnPacketComplete(DYNACORD_EP_MIDI_IN, b.ctx, (int)0xE00002EB /* kIOReturnAborted */, 0);
	CHECK_EQ(Pending(f, DYNACORD_EP_MIDI_IN), before - 1);
}

/* The segment is client-writable: indices outside the ring must be masked on
 * load, never used to index the buffer, and never published back unmasked. */
void test_engine_midi_masks_shm_indices()
{
	EngineFixture f;
	CHECK(f.engine.Start());
	OzzyMidiBuffer* out[2] = { &f.shm->midiOut, &f.shm->midiOutPort1 };
	OzzyMidiBuffer* in[2] = { &f.shm->midiIn, &f.shm->midiInPort1 };
	for (int p = 0; p < 2; p++) {
		out[p]->writeIndex = 0xFFFFFFF0;
		out[p]->readIndex = 0xFFFFFFF0 - 3;
		in[p]->writeIndex = 0xFFFFFFF0;
		in[p]->readIndex = 0xFFFFFFF0 - 5;
	}
	TickOut(f);
	for (int p = 0; p < 2; p++) {
		CHECK(out[p]->readIndex < kOzzyMidiRingSize);
		CHECK(out[p]->writeIndex == 0xFFFFFFF0);   /* the client's, untouched */
	}
	FakeBus::Bulk o = f.bus.PopBulk(DYNACORD_EP_MIDI_OUT);
	f.engine.OnPacketComplete(DYNACORD_EP_MIDI_OUT, o.ctx, 0, 9);

	FakeBus::Bulk b = f.bus.PopBulk(DYNACORD_EP_MIDI_IN);
	const uint8_t pkt[9] = { 0xfd, 0x90, 0x3c, 0x64, 0xfd, 0xb0, 0x07, 0x40, 0xfd };
	std::memcpy(b.buffer, pkt, 9);
	f.engine.OnPacketComplete(DYNACORD_EP_MIDI_IN, b.ctx, 0, 9);
	for (int p = 0; p < 2; p++)
		CHECK(in[p]->writeIndex < kOzzyMidiRingSize);
	CHECK(!f.engine.Failed());
}
