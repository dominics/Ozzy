#include "OzzyMIDI.h"
#include "../../engine/shared/ozzy_shared_data.h"
#include "../Shared/OzzyLog.h"
#include "../Shared/OzzyKextClient.h"
#include "../Shared/OzzyBackend.h"
#include "OzzyMIDIPort2.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#include <unistd.h>
#include <pthread.h>
#include <atomic>
#include <IOKit/IOKitLib.h>

/*
 * One port per OzzyKext instance or OzzyDaemon segment (OzzyBackend.h; a DB4 and a Xone:1D can
 * be connected at the same time).
 * A port keeps its MIDI device when the hardware goes away (it goes offline), so apps keep
 * their connections when the same device comes back.
 *
 * Threads: everything that changes CoreMIDI devices (create, rename, online/offline, dispose)
 * runs on MIDIServer's main run loop, from Start/Stop and a scan timer. MIDIServer calls Stop
 * there while holding its own lock, and those calls take that lock too, so making them on the
 * poll thread deadlocks Stop's join. The poll thread only drains the input rings and calls
 * MIDIReceived.
 */
#define kMaxPorts 8

// MIDI out that doesn't fit the shared ring waits here until the kext has drained it: the
// device takes 2500 bytes/s and the ring holds 1 KB. 64 KB like the official driver's MIDI
// queue (26 s of MIDI).
#define kPendingSize 65536

struct OzzyMidiPort {
	// Connection to one OzzyKext instance or daemon segment; shm is null while disconnected
	uint64_t registryID;   // the OzzyKext instance's, or the USB device's for a daemon
	uint32_t locationID;   // USB port: tells two devices of the same model apart
	OzzyBackend backend;
	ozzy_shared_memory* shm;
	uint32_t sessionID;

	// CoreMIDI objects, kept across reconnects
	MIDIDeviceRef device;
	MIDIEndpointRef source;
	char name[64];

	// Bumped on every connect, so the poll thread knows to reset its parser
	uint32_t generation;

	// MIDI out waiting for room in the shared ring, oldest first (gPortLock); its dropped
	// bytes are reported once it drains
	struct ozzy_midi_queue pending;
	Byte pendingBuf[kPendingSize];

	// The CMS 600-3's second MIDI port (OzzyMIDIPort2.h)
	OzzyMidiPort2 port2;
};

static OzzyMidiPort gPorts[kMaxPorts];
static MIDIDriverRef gDriverRef = nullptr;

// Our devices stay in the MIDI setup across MIDIServer restarts (Start gets them back); this
// property is the USB port each was on, to give it back to the same device
static const CFStringRef kLocationProperty = CFSTR("com.ozzy.locationID");

// Held around every use of a port's shared memory and pending MIDI out by Send and the poll
// thread, and while the main thread maps or unmaps it. Never held across a CoreMIDI call.
static pthread_mutex_t gPortLock = PTHREAD_MUTEX_INITIALIZER;

static std::atomic<bool> gThreadShouldRun{false};
static pthread_t gPollThread = nullptr;
static CFRunLoopTimerRef gScanTimer = nullptr;

/* The name of the device in @shm as a port keeps it: bounded to OzzyMidiPort::name, so the
 * CoreMIDI device, the port and PortForDevice all see the same string */
static const char* PortName(char (&buf)[sizeof(OzzyMidiPort::name)], const ozzy_shared_memory* shm) {
	return OzzyCopyName(buf, shm->product_name, "Ozzy MIDI");
}

/* The device of @port for the device in @shm on USB port @locationID: the port's own (renamed if
 * it was another's), else a new one, added to the MIDI setup */
static void SetPortDevice(OzzyMidiPort* port, const ozzy_shared_memory* shm, uint32_t locationID) {
	char nameBuf[sizeof(port->name)], mfgBuf[sizeof(shm->manufacturer_name) + 1];
	const char* name = PortName(nameBuf, shm);
	const char* mfg = OzzyCopyName(mfgBuf, shm->manufacturer_name, "Ozzy");
	CFStringRef deviceName = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
	CFStringRef vendorName = CFStringCreateWithCString(NULL, mfg, kCFStringEncodingUTF8);

	if (port->device) {
		if (deviceName) MIDIObjectSetStringProperty(port->device, kMIDIPropertyName, deviceName);
		if (vendorName) MIDIObjectSetStringProperty(port->device, kMIDIPropertyManufacturer, vendorName);
	} else {
		LogOzzyMIDI("creating device: '%{public}s'", name);
		OSStatus err = MIDIDeviceCreate(gDriverRef, deviceName, vendorName, deviceName, &port->device);
		MIDIEntityRef ent = 0;
		if (err == noErr) {
			err = MIDIDeviceAddEntity(port->device, CFSTR("MIDI"), false, 1, 1, &ent);
			if (err != noErr) {
				LogOzzyMIDIError("failed to add the MIDI entity: %{public}d", (int)err);
				MIDIDeviceDispose(port->device);
			}
		}
		if (err == noErr) {
			port->source = MIDIEntityGetSource(ent, 0);
			MIDIEndpointSetRefCons(MIDIEntityGetDestination(ent, 0), port, nullptr);
			// Without this the device exists but no client sees it (only a rescan's FindDevices
			// showed it)
			err = MIDISetupAddDevice(port->device);
			if (err != noErr) LogOzzyMIDIError("failed to add the device to the MIDI setup: %{public}d", (int)err);
		} else {
			LogOzzyMIDIError("failed to create device: %{public}d", (int)err);
			port->device = 0;
		}
	}
	if (port->device) {
		MIDIObjectSetIntegerProperty(port->device, kLocationProperty, (SInt32)locationID);
		MIDIObjectSetIntegerProperty(port->device, kMIDIPropertyOffline, 0);
	}
	strlcpy(port->name, name, sizeof(port->name));

	if (deviceName) CFRelease(deviceName);
	if (vendorName) CFRelease(vendorName);
}

/* Close the port's connection to its driver; no CoreMIDI calls */
static void UnmapPort(OzzyMidiPort* port) {
	pthread_mutex_lock(&gPortLock);
	port->backend.Close();
	port->shm = nullptr;
	port->registryID = 0;
	port->pending.head = port->pending.tail = 0;
	port->port2.rings = nullptr;
	port->port2.pending.head = port->port2.pending.tail = 0;
	pthread_mutex_unlock(&gPortLock);
}

/* The port's second entity for @backend's second MIDI port, or none (OzzyMIDIPort2.h) */
static void SetPort2(OzzyMidiPort* port, OzzyBackend& backend) {
	struct ozzy_daemon_midi2* m2 = backend.Midi2();
	MIDIEndpointRef source = port->device ? OzzyMidiSetEntities(port->device, m2, port) : 0;

	pthread_mutex_lock(&gPortLock);
	port->port2.source = source;
	port->port2.rings = source ? m2 : nullptr;
	port->port2.pending.buf = port->port2.pendingBuf;
	port->port2.pending.mask = kPort2PendingSize - 1;
	port->port2.pending.head = port->port2.pending.tail = 0;
	pthread_mutex_unlock(&gPortLock);
	if (source) LogOzzyMIDI("'%{public}s': second MIDI port '%{public}.32s'", port->name, m2->name);
}

static void DisconnectPort(OzzyMidiPort* port) {
	if (!port->shm) return;
	UnmapPort(port);
	if (port->device) MIDIObjectSetIntegerProperty(port->device, kMIDIPropertyOffline, 1);
	LogOzzyMIDI("'%{public}s' disconnected", port->name);
}

static bool PortIsConnected(uint64_t registryID) {
	for (int i = 0; i < kMaxPorts; i++)
		if (gPorts[i].shm && gPorts[i].registryID == registryID) return true;
	return false;
}

/* A free port for a device called @name on USB port @locationID: preferably the one it had
 * before (or one of that name from the setup without a USB port: added by a rescan), else an
 * unused one, else any disconnected port (its device gets renamed). */
static OzzyMidiPort* PortForDevice(const char* name, uint32_t locationID) {
	for (int i = 0; i < kMaxPorts; i++)
		if (!gPorts[i].shm && gPorts[i].device && gPorts[i].locationID == locationID &&
		    strcmp(gPorts[i].name, name) == 0) return &gPorts[i];
	for (int i = 0; i < kMaxPorts; i++)
		if (!gPorts[i].shm && gPorts[i].device && gPorts[i].locationID == 0 &&
		    strcmp(gPorts[i].name, name) == 0) return &gPorts[i];
	for (int i = 0; i < kMaxPorts; i++)
		if (!gPorts[i].shm && !gPorts[i].device) return &gPorts[i];
	for (int i = 0; i < kMaxPorts; i++)
		if (!gPorts[i].shm) return &gPorts[i];
	return nullptr;
}

/* Give an open backend a port, or close it */
static void AttachBackend(OzzyBackend& backend, uint64_t registryID, uint32_t locationID) {
	ozzy_shared_memory* shm = backend.shm;
	OzzyMidiPort* port = nullptr;
	if (shm->magic != OZZY_MAGIC || shm->version != OZZY_SHARED_VERSION) {
		LogOzzyMIDIError("SHM magic/version mismatch (version %{public}u, expected %{public}u): reinstall Ozzy",
		                 shm->version, OZZY_SHARED_VERSION);
	} else if (!shm->audio.hardware_present.load()) {
		// Not started yet; try again on the next scan
	} else {
		char nameBuf[sizeof(OzzyMidiPort::name)];
		port = PortForDevice(PortName(nameBuf, shm), locationID);
		if (!port) LogOzzyMIDIError("too many devices");
	}
	if (!port) {
		backend.Close();
		return;
	}

	SetPortDevice(port, shm, locationID);
	SetPort2(port, backend);

	pthread_mutex_lock(&gPortLock);
	port->registryID = registryID;
	port->locationID = locationID;
	port->backend = backend;
	port->sessionID = shm->session_id;
	port->generation++;
	port->pending.buf = port->pendingBuf;
	port->pending.mask = kPendingSize - 1;
	port->shm = shm;
	pthread_mutex_unlock(&gPortLock);
	LogOzzyMIDI("connected to '%{public}s' (%{public}s, session: 0x%{public}08X)", port->name,
	            backend.kind == OzzyBackendKind::Daemon ? "daemon" : "kext", port->sessionID);
}

static void ConnectService(io_service_t service, uint64_t registryID) {
	OzzyBackend backend;
	if (!backend.OpenKext(service)) {
		LogOzzyMIDIError("failed to open or map the kext");
		return;
	}
	AttachBackend(backend, registryID, OzzyUSBLocationID(service));
}

/* Connect to the daemon segments of listed USB devices that no port has yet (by the kext or a
 * daemon). A device without a segment (no daemon drives it) is skipped quietly; one the console
 * user doesn't own, or from a daemon this plug-in doesn't understand, is refused (logged once). */
static void ScanDaemonSegments() {
	static OzzyDaemonRefusalLog refusals;
	static bool consoleUnknown;
	OzzyUSBDevice usb[kMaxPorts];
	uid_t console;
	bool consoleKnown = OzzyConsoleUser(&console);
	if (!consoleKnown && !consoleUnknown) LogOzzyMIDIError("console user unknown: no daemon segment is mapped");
	consoleUnknown = !consoleKnown;
	if (!consoleKnown) return;
	unsigned int n = OzzyListUSBDevices(usb, kMaxPorts);
	for (unsigned int k = 0; k < n; k++) {
		bool known = false;
		for (int i = 0; i < kMaxPorts; i++)
			known |= gPorts[i].shm && (gPorts[i].registryID == usb[k].registryID ||
			                           gPorts[i].locationID == usb[k].locationID);
		OzzyBackend backend;
		if (known) continue;
		if (backend.OpenDaemonOwnedBy(usb[k].locationID, console))
			AttachBackend(backend, usb[k].registryID, usb[k].locationID);
		else if (refusals.IsNew(usb[k].locationID, backend.refusal, backend.refusalValue)) {
			if (backend.refusal == OzzyDaemonRefusal::Owner)
				LogOzzyMIDIError("ignoring the daemon segment of %{public}08x: owned by uid %{public}u, not the console user %{public}u",
			                 usb[k].locationID, backend.refusalValue, (unsigned int)console);
			else
				LogOzzyMIDIError("ignoring the daemon segment of %{public}08x: version %{public}u, expected %{public}u",
			                 usb[k].locationID, backend.refusalValue, (unsigned int)OZZY_DAEMON_VERSION);
		}
	}
}

/* Connect to OzzyKext instances and daemon segments we don't have yet and drop ports whose
 * device (or daemon) went away */
static void ScanDevices() {
	static unsigned int scans;
	for (int i = 0; i < kMaxPorts; i++) {
		OzzyMidiPort* port = &gPorts[i];
		if (!port->shm) continue;
		if (port->shm->magic != OZZY_MAGIC || port->shm->session_id != port->sessionID ||
		    !port->shm->audio.hardware_present.load(std::memory_order_relaxed) || !port->backend.Alive()) {
			DisconnectPort(port);
		} else if (!port->device) {
			// MIDIDeviceCreate failed when it connected: try again each 500 ms, entities included
			if (scans % 10 == 0) {
				SetPortDevice(port, port->shm, port->locationID);
				if (port->device) SetPort2(port, port->backend);
			}
		} else if (!port->port2.rings && port->backend.Midi2()) {
			SetPort2(port, port->backend);
		}
	}

	io_iterator_t it = IO_OBJECT_NULL;
	if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching("OzzyKext"), &it) != KERN_SUCCESS) return;
	io_service_t service;
	while ((service = IOIteratorNext(it))) {
		uint64_t registryID = 0;
		IORegistryEntryGetRegistryEntryID(service, &registryID);
		if (!PortIsConnected(registryID)) ConnectService(service, registryID);
		IOObjectRelease(service);
	}
	IOObjectRelease(it);
	// Daemon segments each 500 ms: it lists the USB devices
	if (scans++ % 10 == 0) ScanDaemonSegments();
}

/* Drain the port's input rings (midi_in, and the second port's) into CoreMIDI. Returns true if
 * there was data. */
static bool DrainInput(OzzyMidiPort* port, OzzyMidiParsers* parsers) {
	Byte bytes[OZZY_MIDI_RING_SIZE], bytes2[OZZY_MIDI_RING_SIZE];
	uint32_t count = 0, count2 = 0;

	// Copy the rings out under the lock, so the main thread can't unmap them meanwhile
	pthread_mutex_lock(&gPortLock);
	ozzy_shared_memory* shm = port->shm;
	MIDIEndpointRef source = port->source, source2 = port->port2.source;
	uint32_t generation = port->generation;
	if (shm) count = ozzy_midi_ring_pop(&shm->midi_in, bytes, sizeof(bytes));
	if (port->port2.rings) count2 = ozzy_midi_ring_pop(&port->port2.rings->in, bytes2, sizeof(bytes2));
	pthread_mutex_unlock(&gPortLock);

	if (parsers->generation != generation) {
		*parsers = OzzyMidiParsers{};
		parsers->generation = generation;
	}
	OzzyMidiFeed(&parsers->port[0], source, bytes, count);
	OzzyMidiFeed(&parsers->port[1], source2, bytes2, count2);
	return count || count2;
}

/* Move waiting MIDI out into the shared ring as the kext drains it. Returns true if any moved. */
static bool FlushMidiOut(OzzyMidiPort* port) {
	uint32_t dropped = 0;

	pthread_mutex_lock(&gPortLock);
	bool moved = port->shm && ozzy_midi_queue_pending(&port->pending) &&
	             ozzy_midi_queue_write(&port->shm->midi_out, &port->pending, nullptr, 0);
	if (moved) port->backend.MidiOutPending();
	if (!ozzy_midi_queue_pending(&port->pending)) {
		dropped = port->pending.dropped;
		port->pending.dropped = 0;
	}
	if (port->port2.rings && ozzy_midi_queue_pending(&port->port2.pending))
		moved |= ozzy_midi_queue_write(&port->port2.rings->out, &port->port2.pending, nullptr, 0) != 0;
	if (!ozzy_midi_queue_pending(&port->port2.pending)) {
		dropped += port->port2.pending.dropped;
		port->port2.pending.dropped = 0;
	}
	pthread_mutex_unlock(&gPortLock);

	if (dropped) LogOzzyMIDIError("'%{public}s': MIDI out queue drained, %{public}u bytes were dropped", port->name, dropped);
	return moved;
}

static void* MidiInputPollThread(void* arg) {
	LogOzzyMIDI("poll thread started");
	OzzyMidiParsers parsers[kMaxPorts] = {};

	while (gThreadShouldRun.load()) {
		bool didWork = false;
		for (int i = 0; i < kMaxPorts; i++) {
			didWork |= DrainInput(&gPorts[i], &parsers[i]);
			didWork |= FlushMidiOut(&gPorts[i]);
		}
		if (!didWork) usleep(500);
	}
	return NULL;
}

static void ScanTimerFired(CFRunLoopTimerRef, void*) {
	ScanDevices();
}

static OSStatus FindDevices(MIDIDriverRef self, MIDIDeviceListRef devList) {
	for (int i = 0; i < kMaxPorts; i++) {
		if (gPorts[i].device) MIDIDeviceListAddDevice(devList, gPorts[i].device);
	}
	return noErr;
}
static OSStatus Configure(MIDIDriverRef, MIDIDeviceRef) { return noErr; }
static OSStatus EnableSource(MIDIDriverRef, MIDIEndpointRef, Boolean) { return noErr; }
static OSStatus Flush(MIDIDriverRef, MIDIEndpointRef, void*, void*) { return noErr; }
static OSStatus Monitor(MIDIDriverRef, MIDIEndpointRef, const MIDIPacketList*) { return noErr; }

static void CopyName(MIDIObjectRef obj, char* buf, size_t size) {
	CFStringRef name = nullptr;
	buf[0] = 0;
	MIDIObjectGetStringProperty(obj, kMIDIPropertyName, &name);
	if (name) {
		CFStringGetCString(name, buf, (CFIndex)size, kCFStringEncodingUTF8);
		CFRelease(name);
	}
}

/* One of our devices from the setup takes a port, offline until its hardware is found. A second
 * device for the same name and USB port is a duplicate and goes. */
static void AdoptDevice(MIDIDeviceRef dev) {
	char name[64];
	SInt32 location = 0;
	CopyName(dev, name, sizeof(name));
	MIDIObjectGetIntegerProperty(dev, kLocationProperty, &location);
	for (int i = 0; i < kMaxPorts; i++) {
		if (gPorts[i].device == dev) return;
		if (gPorts[i].device && gPorts[i].locationID == (uint32_t)location && strcmp(gPorts[i].name, name) == 0) {
			OSStatus err = MIDISetupRemoveDevice(dev);
			LogOzzyMIDI("removing a duplicate of '%{public}s' from the MIDI setup: %{public}d", name, (int)err);
			return;
		}
	}
	OzzyMidiPort* port = nullptr;
	for (int i = 0; i < kMaxPorts && !port; i++)
		if (!gPorts[i].device) port = &gPorts[i];
	if (!port || MIDIDeviceGetNumberOfEntities(dev) < 1) return;
	MIDIEntityRef ent = MIDIDeviceGetEntity(dev, 0);
	if (MIDIEntityGetNumberOfSources(ent) < 1 || MIDIEntityGetNumberOfDestinations(ent) < 1) return;

	strlcpy(port->name, name, sizeof(port->name));
	port->device = dev;
	port->locationID = (uint32_t)location;
	port->source = MIDIEntityGetSource(ent, 0);
	MIDIEndpointSetRefCons(MIDIEntityGetDestination(ent, 0), port, nullptr);
	port->port2.source = OzzyMidiAdoptSecondEntity(dev, port);
	MIDIObjectSetIntegerProperty(dev, kMIDIPropertyOffline, 1);
	LogOzzyMIDI("'%{public}s' from the MIDI setup", port->name);
}

/* Start: our devices already in the setup (from an earlier MIDIServer run, @devList) take ports;
 * ScanDevices gives each back to its device. Devices with our location property but no driver
 * owner are from builds that set kMIDIPropertyDriverOwner to a number: MIDIServer doesn't hand
 * them back, won't let us remove them, and setting the owner again doesn't make their endpoints
 * ours. They stay offline; Audio MIDI Setup can delete them. */
static void AdoptDevices(MIDIDeviceListRef devList) {
	ItemCount count = devList ? MIDIDeviceListGetNumberOfDevices(devList) : 0;
	for (ItemCount d = 0; d < count; d++)
		AdoptDevice(MIDIDeviceListGetDevice(devList, d));

	for (ItemCount i = 0; i < MIDIGetNumberOfDevices(); i++) {
		MIDIDeviceRef dev = MIDIGetDevice(i);
		SInt32 location = 0;
		CFStringRef owner = nullptr;
		if (MIDIObjectGetIntegerProperty(dev, kLocationProperty, &location) != noErr) continue;
		if (MIDIObjectGetStringProperty(dev, kMIDIPropertyDriverOwner, &owner) == noErr && owner) {
			CFRelease(owner);
			continue;
		}
		char name[64];
		CopyName(dev, name, sizeof(name));
		LogOzzyMIDI("'%{public}s' in the MIDI setup is from an older build; delete it in Audio MIDI Setup", name);
	}
}

static OSStatus Start(MIDIDriverRef self, MIDIDeviceListRef devList) {
	gDriverRef = self;
	AdoptDevices(devList);
	ScanDevices();
	if (!gScanTimer) {
		// Look for new and departed devices every 50 ms
		gScanTimer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 0.05, 0.05, 0, 0, ScanTimerFired, NULL);
		CFRunLoopAddTimer(CFRunLoopGetMain(), gScanTimer, kCFRunLoopCommonModes);
	}
	if (!gThreadShouldRun.load()) {
		gThreadShouldRun.store(true);
		pthread_create(&gPollThread, NULL, MidiInputPollThread, NULL);
	}
	return noErr;
}

static OSStatus Stop(MIDIDriverRef self) {
	if (gScanTimer) {
		CFRunLoopTimerInvalidate(gScanTimer);
		CFRelease(gScanTimer);
		gScanTimer = nullptr;
	}
	if (gThreadShouldRun.load()) {
		gThreadShouldRun.store(false);
		pthread_join(gPollThread, NULL);
		gPollThread = nullptr;
	}
	// Only close the kext connections. MIDIServer stops its drivers when it shuts down, and
	// changing or disposing devices that are in the setup then crashes it (in whichever
	// driver it stops next); the devices stay in the setup as they are.
	for (int i = 0; i < kMaxPorts; i++) {
		if (gPorts[i].shm) UnmapPort(&gPorts[i]);
	}
	return noErr;
}

/* Queue the bytes for the kext (ozzy_midi_queue_write), then tell it. Ploytec devices pick them up
 * with the next PCM packet anyway; the Xone:1D sends only when told. */
static OSStatus Send(MIDIDriverRef self, const MIDIPacketList *pktlist, void *destRefCon1, void *destRefCon2) {
	OzzyMidiPort* port = (OzzyMidiPort*)destRefCon1;
	bool second = destRefCon2 == OzzyMidiEntityRefCon(1);   // the second port's entity
	if (!port) return kMIDINoConnection;

	pthread_mutex_lock(&gPortLock);
	if (!port->shm || (second && !port->port2.rings)) {
		pthread_mutex_unlock(&gPortLock);
		return kMIDINoConnection;
	}

	ozzy_midi_ring* ring = second ? &port->port2.rings->out : &port->shm->midi_out;
	struct ozzy_midi_queue* pending = second ? &port->port2.pending : &port->pending;
	uint32_t droppedBefore = pending->dropped;
	const MIDIPacket *packet = &pktlist->packet[0];
	for (unsigned int i = 0; i < pktlist->numPackets; ++i) {
		ozzy_midi_queue_write(ring, pending, packet->data, packet->length);
		packet = MIDIPacketNext(packet);
	}
	if (!second) port->backend.MidiOutPending();
	bool startedDropping = droppedBefore == 0 && pending->dropped != 0;
	pthread_mutex_unlock(&gPortLock);

	if (startedDropping) LogOzzyMIDIError("'%{public}s': MIDI out queue full (%{public}u KB), dropping", port->name,
	                                      (pending->mask + 1) / 1024);
	return noErr;
}

static HRESULT QueryInterface(void* self, REFIID iid, void** out) {
	CFUUIDBytes interfaceBytes = CFUUIDGetUUIDBytes(kOzzyInterfaceUUID);
	if (memcmp(&iid, &interfaceBytes, sizeof(REFIID)) == 0) {
		*out = self;
		((OzzyMIDIDriver*)self)->_refCount++;
		return 0;
	}
	*out = NULL;
	return 0x80004002;
}
static ULONG AddRef(void* self) { return ++((OzzyMIDIDriver*)self)->_refCount; }
static ULONG Release(void* self) {
	OzzyMIDIDriver* driver = (OzzyMIDIDriver*)self;
	if (--driver->_refCount == 0) { free(driver); return 0; }
	return driver->_refCount;
}

static MIDIDriverInterface gOzzyInterfaceTable = {
	NULL, QueryInterface, AddRef, Release, FindDevices, Start, Stop, Configure, Send, EnableSource, Flush, Monitor
};

extern "C" {
	__attribute__((visibility("default")))
	void* OzzyMIDIFactory(CFAllocatorRef allocator, CFUUIDRef typeID) {
		if (CFEqual(typeID, kOzzyMIDIDriverTypeID)) {
			OzzyMIDIDriver* driver = (OzzyMIDIDriver*)calloc(1, sizeof(OzzyMIDIDriver));
			driver->_vtbl = &gOzzyInterfaceTable;
			driver->_refCount = 1;
			return driver;
		}
		return NULL;
	}
}
