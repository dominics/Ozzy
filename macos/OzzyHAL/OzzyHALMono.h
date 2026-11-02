/* SPDX-License-Identifier: MIT */
/*
 * Mono input devices: one-channel input devices that OzzyHAL publishes next to a model's main
 * device (OzzyChannels.h: the CMS 600-3's MON and AUX sends), so an app that only records
 * channels 1-2 can record them too. Each is a device with one input stream, objects
 * kOzzyObjectMonoDevice + 2i and + 2i + 1 of the main device's slot (OzzyHALExtras.h). It
 * shares the main device's engine, clock, rate and timing, and reads its channel from the
 * shared input ring (OzzyHALDevice::ReadInputChannel). Setting its rate changes the main
 * device's. Its IO doesn't count as the main device's, and it has a timeline of its own
 * (OzzyTimeline.h) on the main device's clock.
 *
 * A configuration change of the main device (a rate or device setting, or a new layout from the
 * driver) restarts the engine under a running mono device, which CoreAudio doesn't stop for it:
 * the mono device reads silence from the start of the main device's change, which then asks
 * CoreAudio for one on each mono device; when that comes (IO stopped), its timeline starts over
 * on the new layout.
 *
 * Templates over the device class, so the dispatch can use OzzyHALDevice's own getters; included
 * by OzzyHAL.mm only. C++ (OzzyHAL).
 */

#ifndef OzzyHALMono_h
#define OzzyHALMono_h

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudio/CoreAudio.h>
#include <string.h>
#include "OzzyHALExtras.h"

static inline bool OzzyIsMonoDevice(uint32_t kind) { return OzzyMonoIndex((int)kind) >= 0 && (kind - kOzzyObjectMonoDevice) % 2 == 0; }
static inline bool OzzyIsMonoObject(uint32_t kind) { return OzzyMonoIndex(kind) >= 0; }

/* A mono input device's ID or its stream's, for the main device @device */
static inline AudioObjectID OzzyMonoDeviceID(AudioObjectID device, int i) { return device + kOzzyObjectMonoDevice + 2 * i; }
static inline AudioObjectID OzzyMonoStreamID(AudioObjectID device, int i) { return OzzyMonoDeviceID(device, i) + 1; }

/* The mono devices to add to the plug-in's device list after @dev's, into @out (null: count) */
template <class Device>
static UInt32 OzzyMonoDeviceList(Device* dev, AudioObjectID* out, UInt32 max) {
    UInt32 n = 0;
    for (unsigned int i = 0; i < dev->Extras().MonoCount(); i++, n++)
        if (out && n < max) out[n] = OzzyMonoDeviceID(dev->GetDeviceID(), (int)i);
    return n;
}

/* The main device's connection changed or it was reconfigured (rate, format): tell the mono ones */
template <class Device>
static void OzzyMonoNotify(Device* dev, AudioServerPlugInHostRef host) {
    if (!host) return;
    AudioObjectPropertyAddress related = { kAudioDevicePropertyRelatedDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    if (dev->IsConnected()) host->PropertiesChanged(host, dev->GetDeviceID(), 1, &related);
    for (unsigned int i = 0; i < dev->Extras().MonoCount(); i++) {
        AudioObjectID id = OzzyMonoDeviceID(dev->GetDeviceID(), (int)i);
        if (!dev->IsConnected()) {
            AudioObjectPropertyAddress alive = { kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
            host->PropertiesChanged(host, id, 1, &alive);
            continue;
        }
        AudioObjectPropertyAddress addr[8] = {
            { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
            { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
            { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
            { kAudioDevicePropertyAvailableNominalSampleRates, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
            { kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain },
            { kAudioDevicePropertyZeroTimeStampPeriod, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
            { kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain },
            related };
        host->PropertiesChanged(host, id, 8, addr);
        AudioObjectPropertyAddress fmt[2] = {
            { kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
            { kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain } };
        host->PropertiesChanged(host, id + 1, 2, fmt);
    }
}

/* The main device's configuration change starts: its mono devices read silence until theirs */
template <class Device>
static void OzzyMonoChangeBegins(Device* dev) {
    for (unsigned int i = 0; i < dev->Extras().MonoCount(); i++) dev->Extras().mMonoChanging[i].store(true);
}

/* The main device's configuration change @action is done: ask CoreAudio for one on each mono device */
template <class Device>
static void OzzyMonoRequestChange(Device* dev, AudioServerPlugInHostRef host, UInt64 action) {
    for (unsigned int i = 0; i < dev->Extras().MonoCount(); i++) {
        if (host && dev->IsConnected() &&
            host->RequestDeviceConfigurationChange(host, OzzyMonoDeviceID(dev->GetDeviceID(), (int)i), action, nullptr) ==
                kAudioHardwareNoError)
            continue;
        dev->Extras().mMonoChanging[i].store(false);
        ozzy_timeline_restart(&dev->Extras().mMonoTimeline[i]);
    }
}

/* CoreAudio stopped the mono device's IO for its change (or aborted it): the engine has switched
 * already, so only its timeline starts over, and its rate and format are new */
template <class Device>
static OSStatus OzzyMonoPerformConfigurationChange(Device* dev, AudioServerPlugInHostRef host, uint32_t kind) {
    int i = OzzyMonoIndex(kind);
    if (!dev->Extras().Mono(i) || !OzzyIsMonoDevice(kind)) return kAudioHardwareBadObjectError;
    ozzy_timeline_restart(&dev->Extras().mMonoTimeline[i]);
    dev->Extras().mMonoChanging[i].store(false);
    if (host) {
        AudioObjectID id = OzzyMonoDeviceID(dev->GetDeviceID(), i);
        AudioObjectPropertyAddress rate = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        host->PropertiesChanged(host, id, 1, &rate);
    }
    return kAudioHardwareNoError;
}

/* The mono device's stream format: the main device's input format with one channel */
template <class Device>
static AudioStreamBasicDescription OzzyMonoFormat(Device* dev) {
    AudioStreamBasicDescription f = dev->GetStreamFormat(false);
    f.mChannelsPerFrame = 1;
    f.mBytesPerFrame = f.mBytesPerPacket = f.mBitsPerChannel / 8;
    return f;
}

/* The main UID / model UID with the suffix; the caller releases it */
static inline CFStringRef OzzyMonoCopyString(CFStringRef base, const char* sep, const char* add) {
    return CFStringCreateWithFormat(NULL, NULL, CFSTR("%@%s%s"), base ? base : CFSTR("Ozzy"), sep, add);
}

static inline OSStatus OzzyMonoWrite(UInt32 inMax, UInt32* outSize, void* outData, const void* value, UInt32 size) {
    if (outData && inMax < size) return kAudioHardwareBadPropertySizeError;
    if (outData) memcpy(outData, value, size);
    if (outSize) *outSize = size;
    return kAudioHardwareNoError;
}

template <class Device>
static Boolean OzzyMonoHasProperty(Device* dev, uint32_t kind, const AudioObjectPropertyAddress* a) {
    if (!dev->Extras().Mono(OzzyMonoIndex(kind))) return false;
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyName:
        case kAudioObjectPropertyOwner: return true;
    }
    if (OzzyIsMonoDevice(kind)) {
        switch (a->mSelector) {
            case kAudioObjectPropertyManufacturer: case kAudioObjectPropertyOwnedObjects:
            case kAudioObjectPropertyControlList: case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID:
            case kAudioDevicePropertyTransportType: case kAudioDevicePropertyIsHidden:
            case kAudioDevicePropertyDeviceIsAlive: case kAudioDevicePropertyDeviceIsRunning:
            case kAudioDevicePropertyDeviceIsRunningSomewhere: case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            case kAudioDevicePropertyNominalSampleRate: case kAudioDevicePropertyAvailableNominalSampleRates:
            case kAudioDevicePropertyStreams: case kAudioDevicePropertyStreamConfiguration:
            case kAudioDevicePropertyLatency: case kAudioDevicePropertySafetyOffset:
            case kAudioDevicePropertyPreferredChannelLayout: case kAudioDevicePropertyPreferredChannelsForStereo:
            case kAudioDevicePropertyZeroTimeStampPeriod: case kAudioDevicePropertyClockDomain:
            case kAudioDevicePropertyRelatedDevices: return true;
            case kAudioObjectPropertyElementName:
                return a->mScope == kAudioObjectPropertyScopeInput && a->mElement == 1;
        }
        return false;
    }
    switch (a->mSelector) {
        case kAudioStreamPropertyDirection: case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel: case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: case kAudioStreamPropertyLatency: return true;
    }
    return false;
}

/* Its rate, and its stream's format (which sets the rate), are settable: they go to the main device */
static inline bool OzzyMonoIsPropertySettable(uint32_t kind, const AudioObjectPropertyAddress* a) {
    if (OzzyIsMonoDevice(kind)) return a->mSelector == kAudioDevicePropertyNominalSampleRate;
    return a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat;
}

/*
 * Property data (@outData null: only the size, for GetPropertyDataSize). The layout's size is
 * that of one channel description.
 */
template <class Device>
static OSStatus OzzyMonoGetPropertyData(Device* dev, uint32_t kind, const AudioObjectPropertyAddress* a, UInt32 inMax,
                                        UInt32* outSize, void* outData) {
    int i = OzzyMonoIndex(kind);
    const struct ozzy_mono_input* mono = dev->Extras().Mono(i);
    if (!mono || !OzzyMonoHasProperty(dev, kind, a)) return kAudioHardwareUnknownPropertyError;
    AudioObjectID id = OzzyMonoDeviceID(dev->GetDeviceID(), i), stream = id + 1;
    UInt32 u32 = 0;
    AudioClassID cls;
    AudioObjectID obj;
    CFStringRef str = nullptr;
    bool input = a->mScope == kAudioObjectPropertyScopeInput || a->mScope == kAudioObjectPropertyScopeGlobal;

    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
            cls = kAudioObjectClassID;
            return OzzyMonoWrite(inMax, outSize, outData, &cls, sizeof(cls));
        case kAudioObjectPropertyClass:
            cls = OzzyIsMonoDevice(kind) ? kAudioDeviceClassID : kAudioStreamClassID;
            return OzzyMonoWrite(inMax, outSize, outData, &cls, sizeof(cls));
        case kAudioObjectPropertyOwner:
            obj = OzzyIsMonoDevice(kind) ? kAudioObjectPlugInObject : id;
            return OzzyMonoWrite(inMax, outSize, outData, &obj, sizeof(obj));
        case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer: case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID: case kAudioObjectPropertyElementName: {
            if (outData && inMax < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
            if (outSize) *outSize = sizeof(CFStringRef);
            if (!outData) return kAudioHardwareNoError;
            switch (a->mSelector) {   // the caller releases it
                case kAudioObjectPropertyName:
                    str = OzzyIsMonoDevice(kind) ? CFStringCreateWithCString(NULL, mono->device_name, kCFStringEncodingUTF8)
                                                 : CFStringCreateWithFormat(NULL, NULL, CFSTR("Ozzy %s Input Stream"), mono->name);
                    break;
                case kAudioObjectPropertyManufacturer:
                    str = dev->GetManufacturer() ? (CFStringRef)CFRetain(dev->GetManufacturer()) : CFSTR("Ozzy"); break;
                case kAudioDevicePropertyDeviceUID: str = OzzyMonoCopyString(dev->GetDeviceUID(), "", mono->uid_suffix); break;
                case kAudioDevicePropertyModelUID: str = OzzyMonoCopyString(dev->GetModelUID(), "", mono->uid_suffix); break;
                default: str = CFStringCreateWithCString(NULL, mono->name, kCFStringEncodingUTF8); break;
            }
            *(CFStringRef*)outData = str;
            return kAudioHardwareNoError;
        }
        case kAudioObjectPropertyOwnedObjects: case kAudioDevicePropertyStreams:
            if (!input) return OzzyMonoWrite(inMax, outSize, outData, &stream, 0);
            return OzzyMonoWrite(inMax, outSize, outData, &stream, sizeof(stream));
        case kAudioObjectPropertyControlList: return OzzyMonoWrite(inMax, outSize, outData, &stream, 0);
        case kAudioDevicePropertyTransportType: u32 = dev->GetTransportType(); break;
        case kAudioDevicePropertyIsHidden: u32 = 0; break;
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: u32 = 0; break;   // input only: never the alert device
        case kAudioDevicePropertyDeviceIsAlive: u32 = dev->IsConnected() ? 1 : 0; break;
        case kAudioDevicePropertyDeviceIsRunning: case kAudioDevicePropertyDeviceIsRunningSomewhere:
            u32 = dev->Extras().mMonoIO[i].load() > 0 ? 1 : 0; break;
        case kAudioDevicePropertyNominalSampleRate: {
            Float64 rate = dev->GetNominalSampleRate();
            return OzzyMonoWrite(inMax, outSize, outData, &rate, sizeof(rate));
        }
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            UInt32 n = dev->CopySampleRates((AudioValueRange*)outData, outData ? inMax / sizeof(AudioValueRange) : 0);
            if (outSize) *outSize = n * sizeof(AudioValueRange);
            return kAudioHardwareNoError;
        }
        case kAudioDevicePropertyStreamConfiguration: {
            AudioBufferList list = {};
            list.mNumberBuffers = input && a->mScope == kAudioObjectPropertyScopeInput ? 1 : 0;
            list.mBuffers[0].mNumberChannels = 1;
            list.mBuffers[0].mDataByteSize = 512 * sizeof(Float32);
            UInt32 size = (UInt32)(offsetof(AudioBufferList, mBuffers) + list.mNumberBuffers * sizeof(AudioBuffer));
            return OzzyMonoWrite(inMax, outSize, outData, &list, size);
        }
        case kAudioDevicePropertyLatency:   // kAudioStreamPropertyLatency too
            // The stream's is in the device's, as for the main device
            u32 = OzzyIsMonoDevice(kind) ? dev->GetLatency(kAudioObjectPropertyScopeInput) : 0; break;
        case kAudioDevicePropertySafetyOffset: u32 = dev->GetSafetyOffset(kAudioObjectPropertyScopeInput); break;
        case kAudioDevicePropertyZeroTimeStampPeriod: u32 = dev->GetZeroTimestampPeriod(); break;
        case kAudioDevicePropertyClockDomain: u32 = dev->GetClockDomain(); break;   // the main device's clock
        case kAudioDevicePropertyRelatedDevices: {
            AudioObjectID related[1 + OZZY_MAX_MONO_INPUTS];
            UInt32 n = dev->Extras().RelatedDevices(related);
            return OzzyMonoWrite(inMax, outSize, outData, related, n * sizeof(AudioObjectID));
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            AudioChannelLayout layout = {};
            layout.mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            layout.mNumberChannelDescriptions = 1;
            layout.mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Mono;
            return OzzyMonoWrite(inMax, outSize, outData, &layout, sizeof(layout));
        }
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            UInt32 pair[2] = { 1, 1 };   // its one channel on both sides
            return OzzyMonoWrite(inMax, outSize, outData, pair, sizeof(pair));
        }
        case kAudioStreamPropertyDirection: u32 = 1; break;   // input
        case kAudioStreamPropertyTerminalType: u32 = kAudioStreamTerminalTypeMicrophone; break;
        case kAudioStreamPropertyStartingChannel: u32 = 1; break;
        case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat: {
            AudioStreamBasicDescription f = OzzyMonoFormat(dev);
            return OzzyMonoWrite(inMax, outSize, outData, &f, sizeof(f));
        }
        case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats: {
            AudioStreamRangedDescription* out = (AudioStreamRangedDescription*)outData;
            UInt32 n = dev->CopyStreamRangedDescriptions(false, out, outData ? inMax / sizeof(AudioStreamRangedDescription) : 0);
            for (UInt32 k = 0; out && k < n; k++) {
                Float64 rate = out[k].mFormat.mSampleRate;
                out[k].mFormat = OzzyMonoFormat(dev);
                out[k].mFormat.mSampleRate = rate;
            }
            if (outSize) *outSize = n * sizeof(AudioStreamRangedDescription);
            return kAudioHardwareNoError;
        }
        default: return kAudioHardwareUnknownPropertyError;
    }
    return OzzyMonoWrite(inMax, outSize, outData, &u32, sizeof(u32));
}

template <class Device>
static OSStatus OzzyMonoSetPropertyData(Device* dev, AudioServerPlugInHostRef host, uint32_t kind,
                                        const AudioObjectPropertyAddress* a, UInt32 size, const void* data) {
    if (!dev->Extras().Mono(OzzyMonoIndex(kind))) return kAudioHardwareBadObjectError;
    if (!OzzyMonoIsPropertySettable(kind, a)) return kAudioHardwareUnsupportedOperationError;
    if (OzzyIsMonoDevice(kind)) {
        if (size < sizeof(Float64) || !data) return kAudioHardwareBadPropertySizeError;
        return dev->SetSampleRate(host, *(const Float64*)data);
    }
    if (size < sizeof(AudioStreamBasicDescription) || !data) return kAudioHardwareBadPropertySizeError;
    return dev->SetSampleRate(host, ((const AudioStreamBasicDescription*)data)->mSampleRate);
}

/* IO, counted per mono device: its timeline restarts when its IO starts and stops, like the main
 * device's, and only then: the main device's IO doesn't touch it */
template <class Device>
static OSStatus OzzyMonoStartIO(Device* dev, uint32_t kind) {
    int i = OzzyMonoIndex(kind);
    if (!dev->Extras().Mono(i)) return kAudioHardwareBadObjectError;
    if (!dev->IsConnected()) return kAudioHardwareNotRunningError;
    if (dev->Extras().mMonoIO[i].fetch_add(1) == 0) ozzy_timeline_restart(&dev->Extras().mMonoTimeline[i]);
    return kAudioHardwareNoError;
}

template <class Device>
static OSStatus OzzyMonoStopIO(Device* dev, uint32_t kind) {
    int i = OzzyMonoIndex(kind);
    if (!dev->Extras().Mono(i)) return kAudioHardwareBadObjectError;
    int io = dev->Extras().mMonoIO[i].load();
    while (io > 0 && !dev->Extras().mMonoIO[i].compare_exchange_weak(io, io - 1)) {}
    if (io == 1) ozzy_timeline_restart(&dev->Extras().mMonoTimeline[i]);
    return kAudioHardwareNoError;
}

/* The main device's clock, on the mono device's own timeline (its IO thread's anchor and seed) */
template <class Device>
static OSStatus OzzyMonoGetZeroTimeStamp(Device* dev, uint32_t kind, Float64* sampleTime, UInt64* hostTime, UInt64* seed) {
    int i = OzzyMonoIndex(kind);
    if (!dev->Extras().Mono(i)) return kAudioHardwareBadObjectError;
    uint64_t s = 0, h = 0, n = 0;
    bool got = false;
    if (!dev->ReadClock(&got, &s, &h) || !ozzy_timeline_anchor(&dev->Extras().mMonoTimeline[i], got, &s, &h, &n))
        return kAudioHardwareNotRunningError;
    if (sampleTime) *sampleTime = (Float64)s;
    if (hostTime) *hostTime = h;
    if (seed) *seed = n;
    return kAudioHardwareNoError;
}

template <class Device>
static OSStatus OzzyMonoDoIOOperation(Device* dev, uint32_t kind, UInt32 operation, UInt32 frames,
                                      const AudioServerPlugInIOCycleInfo* cycle, void* buffer) {
    int i = OzzyMonoIndex(kind);
    const struct ozzy_mono_input* mono = dev->Extras().Mono(i);
    if (!mono) return kAudioHardwareBadObjectError;
    if (operation != kAudioServerPlugInIOOperationReadInput) return kAudioHardwareNoError;
    if (dev->Extras().mMonoChanging[i].load(std::memory_order_relaxed)) {
        memset(buffer, 0, frames * sizeof(float));
        return kAudioHardwareNoError;
    }
    dev->ReadInputChannel(mono->channel, frames, cycle, (float*)buffer, dev->Extras().mMonoScratch[i],
                          kOzzyMonoChunkFrames);
    return kAudioHardwareNoError;
}

#endif
