/* SPDX-License-Identifier: MIT */
/*
 * Where OzzyHAL gets a device's shared memory from, and how it calls its driver:
 * - an OzzyKext instance: IOServiceOpen, IOConnectMapMemory, IOConnectCallScalarMethod
 * - a device the userspace daemon drives (ozzy_daemon.h): its table entry, ozzy_daemon_map and
 *   ozzy_daemon_call
 * The plug-in holds one OzzyBackend per device and uses it the same way for both. Header-only,
 * C++.
 *
 * A daemon that crashed leaves its shared memory as it was in each mapping, so a daemon backend
 * is alive only while the daemon's process runs (ozzy_daemon_alive).
 */

#ifndef OzzyBackend_h
#define OzzyBackend_h

#include <IOKit/IOKitLib.h>
#include "../../engine/shared/ozzy_shared_data.h"
#include "ozzy_daemon.h"

enum class OzzyBackendKind : uint8_t {
    None,
    Kext,      // an OzzyKext user client
    Daemon,    // a device of the userspace daemon's
};

struct OzzyBackend {
    OzzyBackendKind kind = OzzyBackendKind::None;
    ozzy_shared_memory* shm = nullptr;

    // Kext: the user client and its mapping
    io_connect_t connect = IO_OBJECT_NULL;
    mach_vm_address_t address = 0;
    mach_vm_size_t size = 0;

    // Daemon: the device as its table entry names it, and the daemon's process
    uint32_t locationID = 0;
    uint32_t sessionID = 0;
    uint32_t daemonPID = 0;

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

    /* Map the shared memory of the daemon's device @entry (the daemon is @pid). False (and
     * nothing held) if it is gone, or no longer the entry's session. */
    bool OpenDaemon(const ozzy_daemon_entry& entry, uint32_t pid) {
        ozzy_shared_memory* s = ozzy_daemon_map(&entry);
        if (!s) return false;
        kind = OzzyBackendKind::Daemon;
        shm = s;
        locationID = entry.location_id;
        sessionID = entry.session_id;
        daemonPID = pid;
        return true;
    }

    /* Unmap and close. The caller makes sure nothing uses shm any more. */
    void Close() {
        if (kind == OzzyBackendKind::Kext) {
            IOConnectUnmapMemory(connect, 0, mach_task_self(), address);
            IOServiceClose(connect);
        } else if (kind == OzzyBackendKind::Daemon) {
            ozzy_daemon_unmap(shm);
        }
        *this = OzzyBackend();
    }

    /* Whether the driver is still there beyond what the shared memory tells (magic, session_id,
     * hardware_present): always for the kext, whose mapping goes with it; for the daemon, while
     * its process runs */
    bool Alive() const {
        if (kind == OzzyBackendKind::Daemon) return ozzy_daemon_alive(daemonPID);
        return kind == OzzyBackendKind::Kext;
    }

    /* OZZY_METHOD_APPLY_SETTING / OZZY_METHOD_SET_RATE with @count scalars (at most 2): returns
     * once the driver has done it, as the kext's methods do */
    kern_return_t Call(uint32_t method, const uint64_t* in, uint32_t count) {
        if (kind == OzzyBackendKind::Kext)
            return IOConnectCallScalarMethod(connect, method, in, count, nullptr, nullptr);
        if (kind != OzzyBackendKind::Daemon) return kIOReturnNotAttached;
        return ozzy_daemon_call(locationID, sessionID, method, in, count, true);
    }
};

#endif
