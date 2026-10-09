#include "OzzyMIDI.h"
#include "../Shared/OzzySharedData.h"
#include "../Shared/OzzyLog.h"
#include "MidiParser.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <atomic>
#include <errno.h>
#include <IOKit/IOKitLib.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <dispatch/dispatch.h>

/* Threading rule: the poll thread is pure IO. It maps and watches the
 * segment, moves bytes from the in rings to MIDIReceived and keeps the ring
 * indices, and makes no CoreMIDI call other than MIDIReceived: Stop() holds
 * CoreMIDI's mutex while it joins the poll thread. Every device lifecycle
 * and property call is posted to the main queue, and the device state below
 * (gMidiDeviceRef, gSources, gPortCount) is written only on the main thread.
 * Stop() makes no CoreMIDI calls. */

static OzzySharedMemory* gSharedMem = nullptr;
/* Send() runs on CoreMIDI's thread and writes through gSharedMem; it holds
 * this lock while it does. The poll thread takes it to publish or drop the
 * mapping, and unmaps only after dropping it, so Send never touches an
 * unmapped segment. Only the poll thread (or Stop() after joining it) maps
 * and unmaps, so the poll thread itself reads gSharedMem without the lock. */
static pthread_mutex_t gShmLock = PTHREAD_MUTEX_INITIALIZER;
static uint16_t gVendorID = 0, gProductID = 0;   // identity of the mapped device
static MIDIDeviceRef gMidiDeviceRef = 0;
static MIDIDriverRef gDriverRef = nullptr;

static const int kMaxPorts = 2;
static std::atomic<int> gPortCount{0};                      // entities currently on the device
static MIDIEndpointRef gSources[kMaxPorts] = {};
static MidiParser gParsers[kMaxPorts] = {};                 // poll thread only

/* Bumped by Stop() and by every connect, reconfigure or disconnect the poll
 * thread posts. A posted block captures it and does nothing once it is
 * stale, so a block queued before Stop() never touches the device after
 * Stop() has returned. */
static std::atomic<uint32_t> gGeneration{1};
/* The generation whose entities are configured, published by the main
 * thread after it writes gSources and gPortCount. The poll thread reads
 * gSources only while this equals gGeneration; bumping the generation
 * clears it. */
static std::atomic<uint32_t> gEntitiesReady{0};
static int gRequestedPorts = 0;                             // poll thread only: ports last posted

// The port count comes from shared memory the other side can write, so it is clamped to 1..2.
static int WantedPorts() { return (gSharedMem && gSharedMem->midiPortCount >= 2) ? 2 : 1; }

/* The daemon's segment is world-writable by name, so any local user could
 * create it first. Only map one owned by the console user, who runs the
 * daemon. */
static bool OwnedByConsoleUser(const struct stat& sb) {
	uid_t uid = 0;
	CFStringRef name = SCDynamicStoreCopyConsoleUser(NULL, &uid, NULL);
	if (!name) return false;
	CFRelease(name);
	return sb.st_uid == uid;
}

static OzzyMidiBuffer& InRing(int port) { return port == 1 ? gSharedMem->midiInPort1 : gSharedMem->midiIn; }
static OzzyMidiBuffer& OutRing(int port) { return port == 1 ? gSharedMem->midiOutPort1 : gSharedMem->midiOut; }

static void EmitToSource(const uint8_t* msg, size_t len, void* ctx)
{
	MIDIEndpointRef src = (MIDIEndpointRef)(uintptr_t)ctx;
	if (!src)
		return;
	Byte storage[sizeof(MIDIPacketList) + MidiParser::kChunk];
	MIDIPacketList* pl = (MIDIPacketList*)storage;
	MIDIPacket* p = MIDIPacketListInit(pl);
	p = MIDIPacketListAdd(pl, sizeof(storage), p, mach_absolute_time(), len, msg);
	if (p)
		MIDIReceived(src, pl);
}

static std::atomic<bool> gThreadShouldRun{false};
static pthread_t gPollThread = nullptr;
static uint32_t gLastSessionID = 0;

// IOKit connection state
static io_connect_t gKextConnect = IO_OBJECT_NULL;
static mach_vm_address_t gKextMapAddress = 0;
static mach_vm_size_t gKextMapSize = 0;
static bool gIsKextMapping = false;
static int gSHMFd = -1;
static ino_t gSHMInode = 0;

/* Main thread only. The name and manufacturer are set by
 * EnsureMIDIDeviceCreated from its snapshot. */
static void SetDeviceOnline(bool online) {
	if (gMidiDeviceRef) {
		SInt32 offline = online ? 0 : 1;
		MIDIObjectSetIntegerProperty(gMidiDeviceRef, kMIDIPropertyOffline, offline);
	}
}

/* Main thread only. Give the device one entity per port: "MIDI" for one,
 * "USB-MIDI" and "SystemCtrl" for the CMS 600-3. Destinations carry their
 * port number. The poll thread resets the parsers when it posts this. */
static void ConfigureEntities(int want) {
	if (want == gPortCount.load())
		return;
	gPortCount.store(0);
	for (ItemCount i = MIDIDeviceGetNumberOfEntities(gMidiDeviceRef); i-- > 0;)
		MIDIDeviceRemoveEntity(gMidiDeviceRef, MIDIDeviceGetEntity(gMidiDeviceRef, i));
	static const CFStringRef one[] = { CFSTR("MIDI") };
	static const CFStringRef two[] = { CFSTR("USB-MIDI"), CFSTR("SystemCtrl") };
	for (int port = 0; port < want; port++) {
		MIDIEntityRef ent;
		OSStatus err = MIDIDeviceAddEntity(gMidiDeviceRef, want == 2 ? two[port] : one[port], false, 1, 1, &ent);
		if (err != noErr) {
			LogOzzyMIDIError("failed to add MIDI entity %{public}d: %{public}d", port, (int)err);
			gPortCount.store(port);
			return;
		}
		gSources[port] = MIDIEntityGetSource(ent, 0);
		MIDIEndpointSetRefCons(MIDIEntityGetDestination(ent, 0), (void*)(intptr_t)port, nullptr);
	}
	gPortCount.store(want);
	LogOzzyMIDI("%{public}d MIDI port(s)", want);
}

/* What the main thread needs from the segment, copied on the poll thread
 * so the main thread never reads gSharedMem. */
struct DeviceSnapshot {
	char name[64];
	char mfg[64];
	int ports;
};
static_assert(sizeof(DeviceSnapshot::name) == sizeof(OzzySharedMemory::productName), "snapshot name size");
static_assert(sizeof(DeviceSnapshot::mfg) == sizeof(OzzySharedMemory::manufacturerName), "snapshot mfg size");

/* Main thread only. Create the device, or rename an existing one, give it
 * the snapshot's ports and mark it online. */
static void EnsureMIDIDeviceCreated(const DeviceSnapshot& snap) {
	CFStringRef deviceName = CFStringCreateWithCString(NULL, snap.name, kCFStringEncodingUTF8);
	CFStringRef vendorName = CFStringCreateWithCString(NULL, snap.mfg, kCFStringEncodingUTF8);

	if (gMidiDeviceRef != 0) {
		if (deviceName) MIDIObjectSetStringProperty(gMidiDeviceRef, kMIDIPropertyName, deviceName);
		if (vendorName) MIDIObjectSetStringProperty(gMidiDeviceRef, kMIDIPropertyManufacturer, vendorName);
		ConfigureEntities(snap.ports);
		SetDeviceOnline(true);
	} else {
			LogOzzyMIDI("creating device: '%{public}s'", snap.name);
		OSStatus err = MIDIDeviceCreate(gDriverRef, deviceName, vendorName, deviceName, &gMidiDeviceRef);
		
		if (err == noErr) {
			gPortCount.store(0);
			ConfigureEntities(snap.ports);
			SInt32 val = 1;
			MIDIObjectSetIntegerProperty(gMidiDeviceRef, kMIDIPropertyDriverOwner, val);
			SetDeviceOnline(true);
		} else {
				LogOzzyMIDIError("failed to create device: %{public}d", (int)err);
		}
	}
	
	if (deviceName) CFRelease(deviceName);
	if (vendorName) CFRelease(vendorName);
}

/* Poll thread. Snapshot the segment, reset the parsers and post the device
 * coming online (create or rename, configure the ports, set online) to the
 * main thread. Bytes are dropped until that block has run. */
static void PostDeviceOnline() {
	DeviceSnapshot snap = { "Ozzy MIDI", "Ozzy", WantedPorts() };
	// The other side may leave a name unterminated: copy a fixed size and terminate.
	if (gSharedMem->productName[0] != 0) {
		memcpy(snap.name, gSharedMem->productName, sizeof(snap.name) - 1);
		snap.name[sizeof(snap.name) - 1] = 0;
	}
	if (gSharedMem->manufacturerName[0] != 0) {
		memcpy(snap.mfg, gSharedMem->manufacturerName, sizeof(snap.mfg) - 1);
		snap.mfg[sizeof(snap.mfg) - 1] = 0;
	}
	for (int i = 0; i < kMaxPorts; i++) gParsers[i] = MidiParser();
	gRequestedPorts = snap.ports;
	uint32_t gen = gGeneration.fetch_add(1) + 1;
	dispatch_async(dispatch_get_main_queue(), ^{
		if (gGeneration.load() != gen)
			return;
		EnsureMIDIDeviceCreated(snap);
		if (gMidiDeviceRef)
			gEntitiesReady.store(gen, std::memory_order_release);
	});
}

static void PublishMapping(OzzySharedMemory* mem) {
	pthread_mutex_lock(&gShmLock);
	gSharedMem = mem;
	pthread_mutex_unlock(&gShmLock);
}

/* Unpublish and unmap the segment and close its handles. Runs on the poll
 * thread, or in Stop() after the join. Returns whether one was mapped. */
static bool UnmapSharedMemory() {
	OzzySharedMemory* mem = gSharedMem;
	if (mem) {
		PublishMapping(nullptr);   // from here on Send() can't be using the segment
		if (gIsKextMapping) {
			IOConnectUnmapMemory(gKextConnect, 0, mach_task_self(), gKextMapAddress);
			IOServiceClose(gKextConnect);
			gKextConnect = IO_OBJECT_NULL;
			gKextMapAddress = 0;
			gKextMapSize = 0;
			gIsKextMapping = false;
		} else {
			munmap(mem, sizeof(OzzySharedMemory));
			if (gSHMFd != -1) {
				close(gSHMFd);
				gSHMFd = -1;
			}
		}
		LogOzzyMIDI("disconnected");
		return true;
	}
	return false;
}

/* Poll thread. Drop the segment, stop feeding the sources and post the
 * device going offline to the main thread. */
static void DisconnectOnPollThread() {
	if (!UnmapSharedMemory())
		return;
	uint32_t gen = gGeneration.fetch_add(1) + 1;
	dispatch_async(dispatch_get_main_queue(), ^{
		if (gGeneration.load() != gen)
			return;
		SetDeviceOnline(false);
	});
}

/* The engine restarted (a rate change or a halted stream) and published a
 * new session in the same segment. The mapping stays valid, so keep it:
 * reset the parsers and follow a change in the port count, which is posted
 * to the main thread. A different device behind the same segment counts as
 * the device going away. */
static bool OnSessionChange() {
	if (gSharedMem->vendorID != gVendorID || gSharedMem->productID != gProductID) {
		LogOzzyMIDIError("device changed, reconnecting");
		DisconnectOnPollThread();
		return false;
	}
	gLastSessionID = gSharedMem->sessionID;
	LogOzzyMIDI("session changed (0x%{public}08X)", gLastSessionID);
	for (int i = 0; i < kMaxPorts; i++) gParsers[i] = MidiParser();
	if (WantedPorts() != gRequestedPorts) PostDeviceOnline();
	return true;
}

/* A segment was mapped: publish it to Send() and remember whose it is. */
static void Connected(OzzySharedMemory* mem) {
	PublishMapping(mem);
	gLastSessionID = mem->sessionID;
	gVendorID = mem->vendorID;
	gProductID = mem->productID;
}

static bool UpdateConnectionState() {
	if (gSharedMem) {
		if (gSharedMem->magic != kOzzyMagic) {
				LogOzzyMIDIError("bad magic, disconnecting");
			DisconnectOnPollThread();
			return false;
		}
		if (gSharedMem->sessionID != gLastSessionID) return OnSessionChange();
		return true;
	}

	// --- 1. Try OzzyKext (IOKit) ---
	CFMutableDictionaryRef matching = IOServiceMatching("OzzyKext");
	io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, matching);
	
	if (service) {
		LogOzzyMIDI("found OzzyKext service, connecting...");
		kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &gKextConnect);
		IOObjectRelease(service);
		
		if (kr == KERN_SUCCESS) {
			kr = IOConnectMapMemory(gKextConnect, 0, mach_task_self(), &gKextMapAddress, &gKextMapSize, kIOMapAnywhere);
			if (kr == KERN_SUCCESS) {
				gIsKextMapping = true;
				Connected((OzzySharedMemory*)gKextMapAddress);
					LogOzzyMIDI("connected to kext (session: 0x%{public}08X)", gLastSessionID);
				PostDeviceOnline();
				return true;
			} else {
					LogOzzyMIDIError("failed to map kext memory (0x%{public}x)", kr);
				IOServiceClose(gKextConnect);
				gKextConnect = IO_OBJECT_NULL;
			}
		}
	}

	// --- 2. Fallback to Daemon (shm_open) ---
	gIsKextMapping = false;
	int fd = shm_open(kOzzySharedMemName, O_RDWR, 0666);
	if (fd == -1) return false;

	struct stat sb;
	if (fstat(fd, &sb) == -1) { close(fd); return false; }
	if (sb.st_size < sizeof(OzzySharedMemory)) { close(fd); return false; }
	if (!OwnedByConsoleUser(sb)) {
		// Logged once per owner, not on every poll.
		static uid_t lastRefused = (uid_t)-1;
		if (sb.st_uid != lastRefused) {
			LogOzzyMIDIError("ignoring /ozzy_shared_mem owned by uid %{public}u, not the console user", sb.st_uid);
			lastRefused = sb.st_uid;
		}
		close(fd);
		return false;
	}

	gSHMInode = sb.st_ino;
	void* ptr = mmap(NULL, sizeof(OzzySharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	
	if (ptr == MAP_FAILED) {
		close(fd);
		return false;
	}

	gSHMFd = fd;
	Connected((OzzySharedMemory*)ptr);
	
	if (gSharedMem->magic != kOzzyMagic) {
		DisconnectOnPollThread();
		return false;
	}
	
	LogOzzyMIDI("connected via shm_open (session: 0x%{public}08X)", gLastSessionID);
	PostDeviceOnline();
	return true;
}

static void* MidiInputPollThread(void* arg) {
	LogOzzyMIDI("poll thread started");
	int checkCounter = 0;
	
	while (gThreadShouldRun.load()) {
		if (!gSharedMem) {
			if (!UpdateConnectionState()) {
				usleep(50000);
				continue;
			}
		} else {
			// Periodically verify connection is still valid
			if (++checkCounter > 1000) {
				checkCounter = 0;
				bool magicValid = (gSharedMem->magic == kOzzyMagic);
				bool sessionValid = (gSharedMem->sessionID == gLastSessionID);
				bool hwPresent = gSharedMem->audio.hardwarePresent.load(std::memory_order_relaxed);
				
				if (!magicValid || !hwPresent) {
						LogOzzyMIDIError("connection lost (magic:%{public}d hw:%{public}d), reconnecting",
						magicValid, hwPresent);
					DisconnectOnPollThread();
					continue;
				}
				if (!sessionValid && !OnSessionChange()) continue;
				// Repost on a port count change, or once this generation's block
				// has run but configured fewer entities than requested.
				bool configured = gEntitiesReady.load(std::memory_order_acquire) == gGeneration.load(std::memory_order_relaxed);
				if (WantedPorts() != gRequestedPorts || (configured && gPortCount.load() != gRequestedPorts))
					PostDeviceOnline();
			}
			
			// Also check hardware presence on every iteration (fast check)
			if (!gSharedMem->audio.hardwarePresent.load(std::memory_order_relaxed)) {
				LogOzzyMIDIError("device disconnected");
				DisconnectOnPollThread();
				continue;
			}
		}

		if (!gSharedMem) continue;

		// Drain every requested ring. Parse only the ports whose entities the
		// main thread has configured for this generation; drop the rest.
		bool ready = gEntitiesReady.load(std::memory_order_acquire) == gGeneration.load(std::memory_order_relaxed);
		int fed = ready ? gPortCount.load() : 0;
		bool didWork = false;
		for (int port = 0; port < gRequestedPorts; port++) {
			OzzyMidiBuffer& ring = InRing(port);
			// Indices come from memory the other side can write: mask before indexing.
			uint32_t r = std::atomic_load_explicit(&ring.readIndex, std::memory_order_relaxed) & kOzzyMidiRingMask;
			uint32_t w = std::atomic_load_explicit(&ring.writeIndex, std::memory_order_acquire) & kOzzyMidiRingMask;
			if (r == w)
				continue;
			didWork = true;
			while (port < fed && r != w) {
				MidiParserFeed(&gParsers[port], ring.buffer[r], EmitToSource, (void*)(uintptr_t)gSources[port]);
				r = (r + 1) & kOzzyMidiRingMask;
			}
			r = w;
			std::atomic_store_explicit(&ring.readIndex, r, std::memory_order_release);
		}
		if (!didWork)
			usleep(500);
	}

	// Stop() unmaps the segment after joining this thread.
	return NULL;
}

static OSStatus FindDevices(MIDIDriverRef self, MIDIDeviceListRef devList) {
	if (gMidiDeviceRef) MIDIDeviceListAddDevice(devList, gMidiDeviceRef);
	return noErr;
}
static OSStatus Configure(MIDIDriverRef, MIDIDeviceRef) { return noErr; }
static OSStatus EnableSource(MIDIDriverRef, MIDIEndpointRef, Boolean) { return noErr; }
static OSStatus Flush(MIDIDriverRef, MIDIEndpointRef, void*, void*) { return noErr; }
static OSStatus Monitor(MIDIDriverRef, MIDIEndpointRef, const MIDIPacketList*) { return noErr; }

static OSStatus Start(MIDIDriverRef self, MIDIDeviceListRef devList) {
	gDriverRef = self;
	if (!gThreadShouldRun.load()) {
		gThreadShouldRun.store(true);
		pthread_create(&gPollThread, NULL, MidiInputPollThread, NULL);
	}
	return noErr;
}

/* Called on MIDIServer's main thread, which also runs the blocks the poll
 * thread posts, so none of them can run while this does. Bumping the
 * generation after the join makes every block already queued stale.
 * MIDIServer calls this from MIDIDriverMgr::StopMIDI, in the middle of
 * stopping every driver; a CoreMIDI object call made here (setting a
 * property, disposing the device) leaves its setup state broken and the
 * next driver's Stop crashes. So this only drops the segment and resets
 * the plugin's own state. The device stays with the server, and a later
 * Start() reuses gMidiDeviceRef. */
static OSStatus Stop(MIDIDriverRef self) {
	if (gThreadShouldRun.load()) {
		gThreadShouldRun.store(false);
		pthread_join(gPollThread, NULL);
		gPollThread = nullptr;
	}
	gGeneration.fetch_add(1);
	UnmapSharedMemory();
	gRequestedPorts = 0;
	gDriverRef = nullptr;
	return noErr;
}

/* Called on CoreMIDI's thread. Holds gShmLock while it writes to the
 * segment so the poll thread can't unmap it underneath; the poll thread
 * alone connects, so a send before it has is refused. */
static OSStatus Send(MIDIDriverRef self, const MIDIPacketList *pktlist, void *destRefCon1, void *destRefCon2) {
	pthread_mutex_lock(&gShmLock);
	if (!gSharedMem) {
		pthread_mutex_unlock(&gShmLock);
		return kMIDINoConnection;
	}
	
	int port = (int)(intptr_t)destRefCon1;
	if (port < 0 || port >= gPortCount.load() || port >= WantedPorts()) {
		pthread_mutex_unlock(&gShmLock);
		return kMIDIUnknownEndpoint;
	}
	OzzyMidiBuffer& ring = OutRing(port);

	const MIDIPacket *packet = &pktlist->packet[0];
	for (unsigned int i = 0; i < pktlist->numPackets; ++i) {
		for (unsigned int j = 0; j < packet->length; ++j) {
			uint32_t w = std::atomic_load_explicit(&ring.writeIndex, std::memory_order_relaxed) & kOzzyMidiRingMask;
			uint32_t r = std::atomic_load_explicit(&ring.readIndex, std::memory_order_acquire) & kOzzyMidiRingMask;
			uint32_t nextW = (w + 1) & kOzzyMidiRingMask;
			if (nextW != r) {
				ring.buffer[w] = packet->data[j];
				std::atomic_store_explicit(&ring.writeIndex, nextW, std::memory_order_release);
			}
		}
		packet = MIDIPacketNext(packet);
	}
	pthread_mutex_unlock(&gShmLock);
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
