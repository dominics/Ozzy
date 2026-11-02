/* SPDX-License-Identifier: MIT */
/*
 * Fork additions to OzzyMIDI: the CMS 600-3's second MIDI port ("SystemCtrl", the mixer's FX
 * control) as a second entity on the port's MIDI device, and MIDI input parsing for both.
 *
 * Only an OzzyDaemon segment has the second port (OzzyBackend::Midi2(), the trailer's ring pair
 * struct ozzy_daemon_midi2); the kext's shared memory has one. With it, the device's entities
 * are "USB-MIDI" (the DIN sockets, entity 0) and the trailer's name (entity 1); without it, one
 * entity as upstream has it. A destination's refcons are the port and its entity index, so
 * Send can tell them apart.
 *
 * Port 2's MIDI out needs no OZZY_METHOD_MIDI_OUT_PENDING: the isoc engine sends a MIDI transfer
 * each millisecond from its timer anyway. It gets a queue of its own (it is FX control, so
 * smaller than port 1's).
 *
 * Input: common/midi_parser.h, one parser per entity, owned by the poll thread.
 */

#ifndef OzzyMIDIPort2_h
#define OzzyMIDIPort2_h

#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#include "../../common/midi_parser.h"
#include "../../common/devices/ploytec/dynacord_midi.h"
#include "../Shared/OzzyDaemonShared.h"

/* MIDI out for port 2 waiting for room in its ring (a power of two) */
#define kPort2PendingSize 8192

/* The second port of an OzzyMidiPort; rings and pending under gPortLock, source main thread */
struct OzzyMidiPort2 {
	struct ozzy_daemon_midi2* rings;   // in the backend's mapping; null while there is none
	MIDIEndpointRef source;            // entity 1's, kept across reconnects like the device
	struct ozzy_midi_queue pending;
	Byte pendingBuf[kPort2PendingSize];
};

/* The poll thread's input parsers for one port, reset when the port connects again */
struct OzzyMidiParsers {
	uint32_t generation;
	struct midi_parser port[2];
};

/* Destination refCon2 of entity @index */
static inline void* OzzyMidiEntityRefCon(int index) {
	return (void*)(intptr_t)index;
}

static inline void OzzyMidiSetName(MIDIObjectRef obj, const char* name) {
	CFStringRef s = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
	if (s) {
		MIDIObjectSetStringProperty(obj, kMIDIPropertyName, s);
		CFRelease(s);
	}
}

/*
 * Give @device the entities for @m2 (null: none): entity 0 stays (connections to it survive)
 * and is renamed, a second is added, renamed or removed. Returns the second's source, or 0.
 * Main thread (MIDIServer's run loop).
 */
static inline MIDIEndpointRef OzzyMidiSetEntities(MIDIDeviceRef device, struct ozzy_daemon_midi2* m2, void* port) {
	ItemCount have = MIDIDeviceGetNumberOfEntities(device);
	if (have < 1) return 0;
	OzzyMidiSetName(MIDIDeviceGetEntity(device, 0), m2 ? DYNACORD_MIDI_PORT_USB_NAME : "MIDI");
	if (!m2) {
		while (have-- > 1) MIDIDeviceRemoveEntity(device, MIDIDeviceGetEntity(device, have));
		return 0;
	}

	char name[OZZY_DAEMON_MIDI_NAME_MAX + 1] = {};
	memcpy(name, m2->name, OZZY_DAEMON_MIDI_NAME_MAX);
	MIDIEntityRef ent = 0;
	if (have >= 2) {
		ent = MIDIDeviceGetEntity(device, 1);
		OzzyMidiSetName(ent, name);
	} else {
		CFStringRef s = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
		OSStatus err = s ? MIDIDeviceAddEntity(device, s, false, 1, 1, &ent) : -1;
		if (s) CFRelease(s);
		if (err != noErr) return 0;
	}
	MIDIEndpointSetRefCons(MIDIEntityGetDestination(ent, 0), port, OzzyMidiEntityRefCon(1));
	return MIDIEntityGetSource(ent, 0);
}

/* A device from the MIDI setup: its second entity's source (refcons set), or 0 if it has none */
static inline MIDIEndpointRef OzzyMidiAdoptSecondEntity(MIDIDeviceRef device, void* port) {
	if (MIDIDeviceGetNumberOfEntities(device) < 2) return 0;
	MIDIEntityRef ent = MIDIDeviceGetEntity(device, 1);
	if (MIDIEntityGetNumberOfSources(ent) < 1 || MIDIEntityGetNumberOfDestinations(ent) < 1) return 0;
	MIDIEndpointSetRefCons(MIDIEntityGetDestination(ent, 0), port, OzzyMidiEntityRefCon(1));
	return MIDIEntityGetSource(ent, 0);
}

/* One complete message or SysEx chunk (at most MIDI_PARSER_CHUNK bytes) to @ctx, a source */
static inline void OzzyMidiEmit(const uint8_t* msg, uint32_t len, void* ctx) {
	MIDIEndpointRef source = (MIDIEndpointRef)(uintptr_t)ctx;
	MIDIPacketList packetList;
	static_assert(sizeof(packetList.packet[0].data) >= MIDI_PARSER_CHUNK, "a chunk fits one packet");
	MIDIPacket* packet = MIDIPacketListInit(&packetList);
	packet = MIDIPacketListAdd(&packetList, sizeof(packetList), packet, mach_absolute_time(), len, msg);
	if (packet) MIDIReceived(source, &packetList);
}

/* @count bytes from the device into @parser; what it completes goes to @source (0: dropped) */
static inline void OzzyMidiFeed(struct midi_parser* parser, MIDIEndpointRef source, const Byte* bytes, uint32_t count) {
	if (!source) return;
	for (uint32_t i = 0; i < count; i++)
		midi_parser_feed(parser, bytes[i], OzzyMidiEmit, (void*)(uintptr_t)source);
}

#endif
