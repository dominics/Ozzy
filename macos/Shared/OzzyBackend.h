/* SPDX-License-Identifier: MIT */
/*
 * Where OzzyHAL and OzzyMIDI get a device's shared memory from, and how they call its driver:
 * - an OzzyKext instance: IOServiceOpen, IOConnectMapMemory, IOConnectCallScalarMethod
 * - an OzzyDaemon segment, /ozzy.<locationID as 8 hex digits> (OzzyDaemonShared.h): shm_open
 *   and mmap, the methods through the trailer's mailbox
 * The plug-ins hold one OzzyBackend per device and use it the same way for both. Header-only,
 * C++ (the plug-ins).
 *
 * A daemon's segment outlives the daemon: one that crashed or hung leaves magic, session_id
 * and hardware_present as they were in every mapping, and one that hands the device to another
 * user's daemon unlinks the name (the new daemon creates a new object under it). So a daemon
 * backend is alive only while the heartbeat moves (the daemon ticks it each 250 ms) and the
 * trailer is valid; once it isn't, the plug-in unmaps and opens the name again on a later scan.
 * A mailbox call stops the heartbeat while the daemon restarts its engine (about a second, the
 * handshake): while one is in flight, the call's own timeout watches the daemon instead.
 */

#ifndef OzzyBackend_h
#define OzzyBackend_h

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../../engine/shared/ozzy_shared_data.h"
#include "../../common/ozzy_devices.h"
#include "OzzyDaemonShared.h"

enum class OzzyBackendKind : uint8_t {
    None,
    Kext,      // an OzzyKext user client
    Daemon,    // an OzzyDaemon segment
};

/* How long a daemon's heartbeat may stand still before the daemon counts as gone */
static const uint64_t kOzzyDaemonHeartbeatTimeoutNs = 1000000000ull;

struct OzzyBackend {
    OzzyBackendKind kind = OzzyBackendKind::None;
    ozzy_shared_memory* shm = nullptr;

    // Kext: the user client and its mapping
    io_connect_t connect = IO_OBJECT_NULL;
    mach_vm_address_t address = 0;
    mach_vm_size_t size = 0;

    // Daemon: the mapping, its trailer (the mailbox, and fork additions after it), and the
    // heartbeat as Alive() last saw it moving (Alive's thread only). Call() sets callInFlight
    // around a mailbox call and callEndedNs after it (__atomic, from the calling thread).
    void* segment = nullptr;
    size_t segmentSize = 0;
    ozzy_daemon_trailer* trailer = nullptr;
    uint32_t heartbeat = 0;
    uint64_t heartbeatSeenNs = 0;
    uint32_t callInFlight = 0;
    uint64_t callEndedNs = 0;

    bool IsOpen() const { return kind != OzzyBackendKind::None; }

    /* Open and map the OzzyKext instance @service. False (and nothing held) on failure. */
    bool OpenKext(io_service_t service) {
        io_connect_t c = IO_OBJECT_NULL;
        mach_vm_address_t a = 0;
        mach_vm_size_t s = 0;
        if (IOServiceOpen(service, mach_task_self(), 0, &c) != KERN_SUCCESS) return false;
        if (IOConnectMapMemory(c, 0, mach_task_self(), &a, &s, kIOMapAnywhere) != KERN_SUCCESS) {
            IOServiceClose(c);
            return false;
        }
        kind = OzzyBackendKind::Kext;
        connect = c;
        address = a;
        size = s;
        shm = (ozzy_shared_memory*)a;
        return true;
    }

    /*
     * Map the daemon segment for the device at @locationID. False (and nothing held) if there is
     * none, which is the normal state while no daemon drives the device, or it isn't one a
     * daemon this plug-in understands has set up.
     */
    bool OpenDaemon(uint32_t locationID) {
        char name[OZZY_DAEMON_SEGMENT_NAME_MAX];
        uint64_t need = ozzy_daemon_segment_size(sizeof(ozzy_shared_memory));
        struct stat sb;

        ozzy_daemon_segment_name(name, locationID);
        int fd = shm_open(name, O_RDWR, 0);
        if (fd < 0) return false;
        bool sized = fstat(fd, &sb) == 0 && (uint64_t)sb.st_size >= need;
        void* p = sized ? mmap(nullptr, (size_t)need, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
        close(fd);
        if (p == MAP_FAILED) return false;
        ozzy_daemon_trailer* t = ozzy_daemon_trailer_at(p, sizeof(ozzy_shared_memory));
        if (!ozzy_daemon_trailer_valid(t)) {
            munmap(p, (size_t)need);
            return false;
        }
        kind = OzzyBackendKind::Daemon;
        segment = p;
        segmentSize = (size_t)need;
        trailer = t;
        shm = (ozzy_shared_memory*)p;
        heartbeat = shm->heartbeat.load(std::memory_order_relaxed);
        heartbeatSeenNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        return true;
    }

    /* Unmap and close. The caller makes sure nothing uses shm any more. */
    void Close() {
        if (kind == OzzyBackendKind::Kext) {
            IOConnectUnmapMemory(connect, 0, mach_task_self(), address);
            IOServiceClose(connect);
        } else if (kind == OzzyBackendKind::Daemon) {
            munmap(segment, segmentSize);
        }
        *this = OzzyBackend();
    }

    /*
     * Whether the driver is still there, as far as the mapping can tell beyond what upstream
     * checks (magic, session_id, hardware_present): always for the kext, whose mapping goes with
     * it; for a daemon, a valid trailer and a heartbeat that moved within the last second. Call
     * it at least each second (the plug-ins' monitor loops): a heartbeat is only seen moving
     * when it is looked at. CLOCK_UPTIME_RAW stands still in system sleep, so a sleep doesn't
     * count against the daemon.
     */
    bool Alive() {
        if (kind != OzzyBackendKind::Daemon) return kind == OzzyBackendKind::Kext;
        if (!ozzy_daemon_trailer_valid(trailer)) return false;
        if (__atomic_load_n(&callInFlight, __ATOMIC_ACQUIRE)) return true;   // the call times out by itself
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        uint64_t ended = __atomic_load_n(&callEndedNs, __ATOMIC_ACQUIRE);
        if (ended > heartbeatSeenNs) heartbeatSeenNs = ended;   // a call answered: the daemon was there
        uint32_t beat = shm->heartbeat.load(std::memory_order_relaxed);
        if (beat != heartbeat) {
            heartbeat = beat;
            heartbeatSeenNs = now;
            return true;
        }
        return now - heartbeatSeenNs < kOzzyDaemonHeartbeatTimeoutNs;
    }

    /*
     * OZZY_METHOD_APPLY_SETTING / OZZY_METHOD_SET_RATE with @count scalars (at most 2): the
     * kext's method, or the daemon's mailbox (which returns once the daemon has rewritten the
     * shared memory, or kIOReturnTimeout after OZZY_DAEMON_CALL_TIMEOUT_MS). One caller at a time.
     */
    kern_return_t Call(uint32_t method, const uint64_t* in, uint32_t count) {
        if (kind == OzzyBackendKind::Kext)
            return IOConnectCallScalarMethod(connect, method, in, count, nullptr, nullptr);
        if (kind != OzzyBackendKind::Daemon || count > 2) return kIOReturnNotAttached;
        uint32_t args[2] = { 0, 0 };
        for (uint32_t i = 0; i < count; i++) {
            if (in[i] > 0xFFFFFFFFull) return kIOReturnBadArgument;
            args[i] = (uint32_t)in[i];
        }
        __atomic_store_n(&callInFlight, 1u, __ATOMIC_RELEASE);
        kern_return_t kr = ozzy_daemon_call(trailer, method, args, count, OZZY_DAEMON_CALL_TIMEOUT_MS);
        if (kr != kIOReturnTimeout)
            __atomic_store_n(&callEndedNs, clock_gettime_nsec_np(CLOCK_UPTIME_RAW), __ATOMIC_RELEASE);
        __atomic_store_n(&callInFlight, 0u, __ATOMIC_RELEASE);
        return kr;
    }

    /* The second MIDI port's ring pair (the CMS 600-3's "SystemCtrl"), or null: a kext, or a
     * device without one, or a daemon that hasn't published it yet (ask again on a later scan) */
    struct ozzy_daemon_midi2* Midi2() {
        return kind == OzzyBackendKind::Daemon ? ozzy_daemon_midi2(trailer) : nullptr;
    }

    /* OZZY_METHOD_MIDI_OUT_PENDING: midi_out has bytes */
    void MidiOutPending() {
        if (kind == OzzyBackendKind::Kext)
            IOConnectCallScalarMethod(connect, OZZY_METHOD_MIDI_OUT_PENDING, nullptr, 0, nullptr, nullptr);
        else if (kind == OzzyBackendKind::Daemon)
            ozzy_daemon_midi_out_pending(trailer);
    }
};

/* A USB device a daemon may publish a segment for */
struct OzzyUSBDevice {
    uint64_t registryID;    // the IOUSBHostDevice's: stays the same until it is unplugged
    uint32_t locationID;    // the segment's name
    const struct ozzy_device* model;
};

static inline uint32_t OzzyRegistryNumber(io_registry_entry_t entry, CFStringRef key) {
    uint32_t n = 0;
    CFTypeRef value = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
    if (value) {
        if (CFGetTypeID(value) == CFNumberGetTypeID())
            CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &n);
        CFRelease(value);
    }
    return n;
}

/*
 * The connected USB devices in the device table (ozzy_devices.def), up to @max of them into
 * @out; returns how many. Where a daemon segment can be: the daemon names its segment by the
 * device's locationID, so a plug-in tries each of these.
 */
static inline unsigned int OzzyListUSBDevices(OzzyUSBDevice* out, unsigned int max) {
    io_iterator_t it = IO_OBJECT_NULL;
    unsigned int count = 0;
    if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching("IOUSBHostDevice"), &it) != KERN_SUCCESS)
        return 0;
    io_service_t service;
    while ((service = IOIteratorNext(it))) {
        const struct ozzy_device* model = ozzy_device_lookup(
            (uint16_t)OzzyRegistryNumber(service, CFSTR("idVendor")),
            (uint16_t)OzzyRegistryNumber(service, CFSTR("idProduct")));
        uint32_t locationID = OzzyRegistryNumber(service, CFSTR("locationID"));
        uint64_t registryID = 0;
        if (model && locationID && count < max &&
            IORegistryEntryGetRegistryEntryID(service, &registryID) == KERN_SUCCESS)
            out[count++] = OzzyUSBDevice { registryID, locationID, model };
        IOObjectRelease(service);
    }
    IOObjectRelease(it);
    return count;
}

#endif
