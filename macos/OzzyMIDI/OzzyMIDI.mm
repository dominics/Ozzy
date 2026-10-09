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
static MidiParser gParsers[kMaxPorts] = {};

// The port count comes from shared memory the other side can write, so it is clamped to 1..2.
static int WantedPorts() { return (gSharedMem && gSharedMem->midiPortCount >= 2) ? 2 : 1; }

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

static void SetDeviceOnline(bool online) {
	if (gMidiDeviceRef) {
		SInt32 offline = online ? 0 : 1;
		MIDIObjectSetIntegerProperty(gMidiDeviceRef, kMIDIPropertyOffline, offline);
		
		if (online && gSharedMem && gSharedMem->productName[0] != 0) {
			CFStringRef newName = CFStringCreateWithCString(NULL, gSharedMem->productName, kCFStringEncodingUTF8);
			if (newName) {
				MIDIObjectSetStringProperty(gMidiDeviceRef, kMIDIPropertyName, newName);
				CFRelease(newName);
			}
			
			CFStringRef newMfg = CFStringCreateWithCString(NULL, gSharedMem->manufacturerName, kCFStringEncodingUTF8);
			if (newMfg) {
				MIDIObjectSetStringProperty(gMidiDeviceRef, kMIDIPropertyManufacturer, newMfg);
				CFRelease(newMfg);
			}
		}
	}
}

/* Give the device one entity per port: "MIDI" for one, "USB-MIDI" and
 * "SystemCtrl" for the CMS 600-3. Destinations carry their port number. */
static void ConfigureEntities() {
	int want = WantedPorts();
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
		gParsers[port] = MidiParser();
	}
	gPortCount.store(want);
	LogOzzyMIDI("%{public}d MIDI port(s)", want);
}

static void EnsureMIDIDeviceCreated() {
	char nameBuf[64] = "Ozzy MIDI";
	char mfgBuf[64] = "Ozzy";

	if (gSharedMem) {
		if (gSharedMem->productName[0] != 0) strlcpy(nameBuf, gSharedMem->productName, 64);
		if (gSharedMem->manufacturerName[0] != 0) strlcpy(mfgBuf, gSharedMem->manufacturerName, 64);
	}

	CFStringRef deviceName = CFStringCreateWithCString(NULL, nameBuf, kCFStringEncodingUTF8);
	CFStringRef vendorName = CFStringCreateWithCString(NULL, mfgBuf, kCFStringEncodingUTF8);

	if (gMidiDeviceRef != 0) {
		if (deviceName) MIDIObjectSetStringProperty(gMidiDeviceRef, kMIDIPropertyName, deviceName);
		if (vendorName) MIDIObjectSetStringProperty(gMidiDeviceRef, kMIDIPropertyManufacturer, vendorName);
		for (int i = 0; i < kMaxPorts; i++) gParsers[i] = MidiParser();
		ConfigureEntities();
		SetDeviceOnline(true);
	} else {
			LogOzzyMIDI("creating device: '%{public}s'", nameBuf);
		OSStatus err = MIDIDeviceCreate(gDriverRef, deviceName, vendorName, deviceName, &gMidiDeviceRef);
		
		if (err == noErr) {
			gPortCount.store(0);
			ConfigureEntities();
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

static void PublishMapping(OzzySharedMemory* mem) {
	pthread_mutex_lock(&gShmLock);
	gSharedMem = mem;
	pthread_mutex_unlock(&gShmLock);
}

static void DisconnectSharedMemory() {
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
		SetDeviceOnline(false);
	}
}

/* The engine restarted (a rate change or a halted stream) and published a
 * new session in the same segment. The mapping stays valid, so keep it:
 * reset the parsers and follow a change in the port count. A different
 * device behind the same segment counts as the device going away. */
static bool OnSessionChange() {
	if (gSharedMem->vendorID != gVendorID || gSharedMem->productID != gProductID) {
		LogOzzyMIDIError("device changed, reconnecting");
		DisconnectSharedMemory();
		return false;
	}
	gLastSessionID = gSharedMem->sessionID;
	LogOzzyMIDI("session changed (0x%{public}08X)", gLastSessionID);
	for (int i = 0; i < kMaxPorts; i++) gParsers[i] = MidiParser();
	ConfigureEntities();
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
			DisconnectSharedMemory();
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
				EnsureMIDIDeviceCreated();
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

	gSHMInode = sb.st_ino;
	void* ptr = mmap(NULL, sizeof(OzzySharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	
	if (ptr == MAP_FAILED) {
		close(fd);
		return false;
	}

	gSHMFd = fd;
	Connected((OzzySharedMemory*)ptr);
	
	if (gSharedMem->magic != kOzzyMagic) {
		DisconnectSharedMemory();
		return false;
	}
	
	LogOzzyMIDI("connected via shm_open (session: 0x%{public}08X)", gLastSessionID);
	EnsureMIDIDeviceCreated();
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
					DisconnectSharedMemory();
					continue;
				}
				if (!sessionValid && !OnSessionChange()) continue;
				if (WantedPorts() != gPortCount.load()) ConfigureEntities();
			}
			
			// Also check hardware presence on every iteration (fast check)
			if (!gSharedMem->audio.hardwarePresent.load(std::memory_order_relaxed)) {
				LogOzzyMIDIError("device disconnected");
				DisconnectSharedMemory();
				continue;
			}
		}

		if (!gSharedMem) continue;

		bool didWork = false;
		for (int port = 0; port < gPortCount.load(); port++) {
			OzzyMidiBuffer& ring = InRing(port);
			// Indices come from memory the other side can write: mask before indexing.
			uint32_t r = std::atomic_load_explicit(&ring.readIndex, std::memory_order_relaxed) & kOzzyMidiRingMask;
			uint32_t w = std::atomic_load_explicit(&ring.writeIndex, std::memory_order_acquire) & kOzzyMidiRingMask;
			if (r == w)
				continue;
			didWork = true;
			while (r != w) {
				MidiParserFeed(&gParsers[port], ring.buffer[r], EmitToSource, (void*)(uintptr_t)gSources[port]);
				r = (r + 1) & kOzzyMidiRingMask;
			}
			std::atomic_store_explicit(&ring.readIndex, r, std::memory_order_release);
		}
		if (!didWork)
			usleep(500);
	}

	DisconnectSharedMemory();
	if (gMidiDeviceRef) { MIDIDeviceDispose(gMidiDeviceRef); gMidiDeviceRef = 0; }
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

static OSStatus Stop(MIDIDriverRef self) {
	if (gThreadShouldRun.load()) {
		gThreadShouldRun.store(false);
		pthread_join(gPollThread, NULL);
		gPollThread = nullptr;
	}
	DisconnectSharedMemory();
	if (gMidiDeviceRef) { MIDIDeviceDispose(gMidiDeviceRef); gMidiDeviceRef = 0; }
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
