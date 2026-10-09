#ifndef IOzzyBus_h
#define IOzzyBus_h

#include <stdint.h>

class OzzyEngine;

// One service interval of an isochronous transfer: requested length in,
// actual length and status out (status 0 = success).
struct OzzyIsochFrame {
    uint16_t reqCount;
    uint16_t actCount;
    int32_t  status;
};

class IOzzyBus {
public:
    virtual ~IOzzyBus() {}

    // --- Core Transport ---
    // Returns true if successfully queued.
    // contextID: A generic tag (usually packet index) passed back in OnPacketComplete
    virtual bool SubmitUSBPacket(uint8_t pipeID, void* buffer, uint32_t size, void* contextID) = 0;
    
    // Blocking Control Request (Used for Initialization)
    // type: bmRequestType, req: bRequest
    virtual bool VendorRequest(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void* data, uint16_t len) = 0;

    /*
     * Queue an isochronous transfer of frameCount service intervals; frame i
     * carries frames[i].reqCount bytes, packed back to back in buffer. The
     * bus picks the start frame, keeping consecutive submissions on a pipe
     * contiguous. frames must stay valid until OnIsochComplete. Buses
     * without isochronous support return false.
     */
    virtual bool SubmitIsochPacket(uint8_t /*pipeID*/, void* /*buffer*/, OzzyIsochFrame* /*frames*/, uint32_t /*frameCount*/, void* /*contextID*/) { return false; }

    /*
     * The current USB bus frame (1 ms units) and the host time at which that
     * frame number was read, from the host controller. The time is 0-1 ms
     * after the frame started; callers wanting the frame start keep the
     * sample with the smallest hostTime - frame * ticks-per-ms. False if the
     * bus can't tell.
     */
    virtual bool GetBusFrame(uint64_t* /*frame*/, uint64_t* /*hostTime*/) { return false; }

    // --- System Services ---
    virtual void Sleep(uint32_t ms) = 0;
    virtual uint64_t GetTime() = 0; // Returns mach_absolute_time()
    virtual void Log(const char* fmt, ...) = 0;
    
    // --- Linkage ---
    virtual void SetEngine(OzzyEngine* engine) = 0;
};

#endif