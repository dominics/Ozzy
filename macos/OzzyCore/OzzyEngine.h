#ifndef OzzyEngine_h
#define OzzyEngine_h

#include "IOzzyBus.h"
#include "../Shared/OzzySharedData.h"

// --- Hardware Description ---
struct OzzyPipeConfig {
    uint8_t address;        // e.g., 0x05
    uint8_t interfaceIndex; // Which interface owns this pipe? (0, 1, etc.)
};

struct OzzyDeviceProfile {
    // Interfaces the driver must claim
    uint8_t interfaceCount = 0;
    uint8_t interfaceIndices[4] = {}; // Max 4 interfaces is plenty for audio
    uint8_t altSettings[4] = {};      // Alternate setting to select per claimed interface

    // Pipe Maps (0 = Unused)
    OzzyPipeConfig pcmOut = {};
    OzzyPipeConfig pcmIn = {};
    OzzyPipeConfig midiIn = {};
    OzzyPipeConfig midiOut = {};      // Bulk MIDI out
    OzzyPipeConfig isoOut = {};       // Isochronous PCM out
    OzzyPipeConfig feedbackIn = {};   // Isochronous rate feedback in
};

// --- Base Class ---
class OzzyEngine {
protected:
    IOzzyBus* mBus = nullptr;
    void* mSharedMemory = nullptr; // Opaque pointer to OzzySharedMemory

public:
    virtual ~OzzyEngine() {}

    // Initialization (Implemented in .cpp)
    virtual void Init(IOzzyBus* bus, void* sharedMemory);

    // New: The Kext calls this to ask "What hardware do you need?"
    virtual const OzzyDeviceProfile& GetProfile() const = 0;

    // Called by kext after probing endpoint descriptors, before Start()
    virtual void SetBulk(bool isBulk) = 0;

    // Lifecycle (Pure Virtual)
    virtual bool Start() = 0;
    virtual void Stop() = 0;

    /* Shared memory layout this engine writes (OzzyLayoutVersion). */
    virtual uint32_t LayoutVersion() const { return kOzzyLayoutPackets; }

    enum class PollResult { kRunning, kRestart };

    /*
     * Called every 100 ms by the host (kext timer or daemon session), with
     * the engine lock held. kRestart: Stop, then Start again at NextRate().
     */
    virtual PollResult Poll() { return PollResult::kRunning; }

    /* Rate for the next Start(), and the rate a kRestart should use. */
    virtual void SetRate(uint32_t /*rate*/) {}
    virtual uint32_t NextRate() const { return 0; }

    // Callbacks
    virtual void OnPacketComplete(uint8_t pipeID, void* contextID, int status, uint32_t bytesTransferred) = 0;

    /*
     * An isochronous transfer finished. startFrame is the bus frame (1 ms)
     * its first service interval was scheduled in; frames[] now holds each
     * interval's actual length and status. Default: ignore.
     */
    virtual void OnIsochComplete(uint8_t /*pipeID*/, void* /*contextID*/, int /*status*/, uint64_t /*startFrame*/, OzzyIsochFrame* /*frames*/, uint32_t /*frameCount*/) {}
};

#endif