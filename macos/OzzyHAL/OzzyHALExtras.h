/* SPDX-License-Identifier: MIT */
/*
 * The fork's additions to an OzzyHAL device, kept out of OzzyHAL.mm, which only calls in here:
 * - output volume and mute controls (OzzyVolume.h), applied where the HAL writes the output
 *   ring, as each device's objects kOzzyObjectVolumeControl and kOzzyObjectMuteControl
 * - channel names (kAudioObjectPropertyElementName), speaker labels in the preferred channel
 *   layout and a preferred stereo pair per direction, per device-table model (OzzyChannels.h)
 * - mono input devices next to the main one (the CMS 600-3's MON and AUX, OzzyChannels.h), as
 *   each device's objects from kOzzyObjectMonoDevice: their state here, their properties and
 *   IO in OzzyHALMono.h
 * - the related devices (kAudioDevicePropertyRelatedDevices) of the main and mono devices: all
 *   of them, so an aggregate of them needs no resampling
 * Volume, mute and the stereo pairs are kept in CoreAudio's plug-in storage per device UID, like
 * the device settings. C++ (OzzyHAL).
 */

#ifndef OzzyHALExtras_h
#define OzzyHALExtras_h

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <atomic>
#include <string.h>
#include "OzzyChannels.h"
#include "OzzyHALObjects.h"
#include "OzzyVolume.h"
#include "OzzyTimeline.h"
#include "../../engine/shared/ozzy_shared_data.h"

/* Frames a mono input device decodes at a time (OzzyHALMono.h) */
static const uint32_t kOzzyMonoChunkFrames = 256;
static const uint32_t kOzzyMonoMaxChannels = OZZY_MONO_MAX_CHANNELS;

/* The controls each device has, in its control list after the clock source */
static const UInt32 kOzzyExtraControls = 2;

/*
 * The driver's layout as the device last applied it (ApplyDeviceConfig), as the shared memory
 * had it: the rate, the rings, the clock spacing and the safety offsets. The monitor compares
 * the shared memory with it, so a layout the driver published late (a rate change that outlived
 * the mailbox's timeout) is applied too.
 */
struct OzzyLayout {
    uint32_t rate, outRingFrames, inRingFrames, timestampFrames, safetyOffset, inputSafetyOffset;

    static OzzyLayout Of(const ozzy_shared_memory* shm) {
        return OzzyLayout { shm->audio.sample_rate.load(std::memory_order_relaxed), shm->audio.output_ring_frames,
                            shm->audio.input_ring_frames, shm->audio.timestamp_frames,
                            shm->audio.safety_offset_frames, shm->audio.input_safety_offset_frames };
    }
    bool operator==(const OzzyLayout& o) const {
        return rate == o.rate && outRingFrames == o.outRingFrames && inRingFrames == o.inRingFrames &&
               timestampFrames == o.timestampFrames && safetyOffset == o.safetyOffset &&
               inputSafetyOffset == o.inputSafetyOffset;
    }
    bool operator!=(const OzzyLayout& o) const { return !(*this == o); }
};

class OzzyHALExtras {
public:
    /* The main device's object ID (OzzyHALDevice::Init) */
    void SetDevice(AudioObjectID device) { mDevice = device; }

    /* The main device and its mono devices into @out; returns how many */
    UInt32 RelatedDevices(AudioObjectID out[1 + OZZY_MAX_MONO_INPUTS]) const {
        return ozzy_related_devices(mDevice, mMonoCount, out);
    }

    /* The device's model and channels, from ApplyDeviceConfig */
    void Configure(const struct ozzy_device* model, uint32_t inputs, uint32_t outputs) {
        mModel = model;
        mInputs = inputs;
        mOutputs = outputs;
        mMonoCount = inputs <= kOzzyMonoMaxChannels ? ozzy_mono_inputs_for(model, mMono) : 0;
    }

    // --- Mono input devices (OzzyHALMono.h) ---

    /* Mono input @i, or null if the device has none at that index */
    const struct ozzy_mono_input* Mono(int i) const {
        return i >= 0 && (unsigned int)i < mMonoCount ? mMono[i] : nullptr;
    }
    unsigned int MonoCount() const { return mMonoCount; }
    /* Its IO: clients that started it, and its own timeline (OzzyTimeline.h) */
    std::atomic<int> mMonoIO[OZZY_MAX_MONO_INPUTS] = {};
    struct ozzy_timeline mMonoTimeline[OZZY_MAX_MONO_INPUTS] = {};
    /* A configuration change of the main device restarted the engine under it, and its own
     * change hasn't come yet: its IO reads silence (OzzyHALMono.h) */
    std::atomic<bool> mMonoChanging[OZZY_MAX_MONO_INPUTS] = {};
    /* Any thread: the mono devices' timelines start over (the driver went away) */
    void RestartMonoTimelines() {
        for (unsigned int i = 0; i < OZZY_MAX_MONO_INPUTS; i++) ozzy_timeline_restart(&mMonoTimeline[i]);
    }
    /* Its IO thread's decode buffer (each device has an IO thread of its own) */
    float mMonoScratch[OZZY_MAX_MONO_INPUTS][kOzzyMonoChunkFrames * kOzzyMonoMaxChannels];

    /* The device came up as @uid: what storage has for it (defaults where it has nothing) */
    void Restore(AudioServerPlugInHostRef host, CFStringRef uid) {
        if (mUID) CFRelease(mUID);
        mUID = uid ? (CFStringRef)CFRetain(uid) : nullptr;
        float volume = 1.0f;
        bool muted = false;
        uint32_t pairs[2] = { kDefaultPair, kDefaultPair };
        CFPropertyListRef v;
        if ((v = CopyOrMigrate(host, kVolumeKey, kLegacyVolumeKey))) {
            if (CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberFloat32Type, &volume);
            CFRelease(v);
        }
        if ((v = CopyOrMigrate(host, kMuteKey, kLegacyMuteKey))) {
            if (CFGetTypeID(v) == CFBooleanGetTypeID()) muted = CFBooleanGetValue((CFBooleanRef)v);
            CFRelease(v);
        }
        for (int out = 0; out < 2; out++) {
            if (!(v = Copy(host, out ? kOutPairKey : kInPairKey))) continue;
            SInt32 n = 0;
            if (CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue((CFNumberRef)v, kCFNumberSInt32Type, &n))
                pairs[out] = (uint32_t)n;
            CFRelease(v);
        }
        for (unsigned int i = 0; i < OZZY_MAX_MONO_INPUTS; i++) {
            mMonoIO[i].store(0);
            mMonoChanging[i].store(false);
        }
        RestartMonoTimelines();
        mVolume.store(ozzy_volume_clamp(volume));
        mMuted.store(muted);
        mPairIn.store(pairs[0]);
        mPairOut.store(pairs[1]);
    }

    /* IO thread, WriteMix: the volume and mute on @frames frames of @channels, in place */
    void ApplyOutputGain(float* samples, uint32_t frames, uint32_t channels) {
        float gain = mMuted.load(std::memory_order_relaxed) ? 0.0f : ozzy_volume_gain(mVolume.load(std::memory_order_relaxed));
        if (frames) ozzy_volume_ramp(samples, frames, channels, mAppliedGain, gain);
        mAppliedGain = gain;
    }

    /* The label of channel @index (0-based) in @scope's preferred layout */
    AudioChannelLabel ChannelLabel(AudioObjectPropertyScope scope, UInt32 index) const {
        const struct ozzy_channel_info* info = ozzy_channel(mModel, IsOutput(scope), index + 1);
        if (!info) return kAudioChannelLabel_Unknown;
        switch (info->role) {
            case OZZY_CHANNEL_LEFT: return kAudioChannelLabel_Left;
            case OZZY_CHANNEL_RIGHT: return kAudioChannelLabel_Right;
            default: return kAudioChannelLabel_Discrete_0 + index;
        }
    }

    // --- The device's own properties (OzzyHAL.mm asks here first; false: not one of ours) ---

    bool HasDeviceProperty(const AudioObjectPropertyAddress* a, Boolean* has) const {
        if (a->mSelector == kAudioDevicePropertyPreferredChannelsForStereo ||
            a->mSelector == kAudioDevicePropertyRelatedDevices) {
            *has = true;
            return true;
        }
        if (a->mSelector == kAudioObjectPropertyElementName) {
            *has = ChannelName(a) != nullptr;
            return true;
        }
        return false;
    }

    static bool IsDevicePropertySettable(const AudioObjectPropertyAddress* a) {
        return a->mSelector == kAudioDevicePropertyPreferredChannelsForStereo;
    }

    bool GetDevicePropertyDataSize(const AudioObjectPropertyAddress* a, UInt32* size, OSStatus* status) const {
        if (a->mSelector == kAudioDevicePropertyPreferredChannelsForStereo) {
            *size = 2 * sizeof(UInt32);
            *status = kAudioHardwareNoError;
            return true;
        }
        if (a->mSelector == kAudioDevicePropertyRelatedDevices) {
            *size = (1 + mMonoCount) * sizeof(AudioObjectID);
            *status = kAudioHardwareNoError;
            return true;
        }
        if (a->mSelector == kAudioObjectPropertyElementName) {
            *size = sizeof(CFStringRef);
            *status = ChannelName(a) ? kAudioHardwareNoError : kAudioHardwareUnknownPropertyError;
            return true;
        }
        return false;
    }

    bool GetDevicePropertyData(const AudioObjectPropertyAddress* a, UInt32 inMax, UInt32* outSize, void* outData,
                               OSStatus* status) const {
        if (a->mSelector == kAudioDevicePropertyPreferredChannelsForStereo) {
            struct ozzy_stereo_pair p = Pair(a->mScope);
            *status = kAudioHardwareNoError;
            if (outData && inMax < 2 * sizeof(UInt32)) *status = kAudioHardwareBadPropertySizeError;
            else {
                if (outData) { ((UInt32*)outData)[0] = p.left; ((UInt32*)outData)[1] = p.right; }
                if (outSize) *outSize = 2 * sizeof(UInt32);
            }
            return true;
        }
        if (a->mSelector == kAudioDevicePropertyRelatedDevices) {
            AudioObjectID related[1 + OZZY_MAX_MONO_INPUTS];
            UInt32 size = RelatedDevices(related) * sizeof(AudioObjectID);
            *status = kAudioHardwareNoError;
            if (outData && inMax < size) *status = kAudioHardwareBadPropertySizeError;
            else {
                if (outData) memcpy(outData, related, size);
                if (outSize) *outSize = size;
            }
            return true;
        }
        if (a->mSelector == kAudioObjectPropertyElementName) {
            const char* name = ChannelName(a);
            *status = kAudioHardwareNoError;
            if (!name) *status = kAudioHardwareUnknownPropertyError;
            else if (outData && inMax < sizeof(CFStringRef)) *status = kAudioHardwareBadPropertySizeError;
            else {
                // The caller releases it
                if (outData) *(CFStringRef*)outData = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
                if (outSize) *outSize = sizeof(CFStringRef);
            }
            return true;
        }
        return false;
    }

    bool SetDevicePropertyData(AudioServerPlugInHostRef host, AudioObjectID device, const AudioObjectPropertyAddress* a,
                               UInt32 size, const void* data, OSStatus* status) {
        if (a->mSelector != kAudioDevicePropertyPreferredChannelsForStereo) return false;
        if (size < 2 * sizeof(UInt32) || !data) {
            *status = kAudioHardwareBadPropertySizeError;
            return true;
        }
        struct ozzy_stereo_pair p = { ((const UInt32*)data)[0], ((const UInt32*)data)[1] };
        bool output = IsOutput(a->mScope);
        if (!ozzy_stereo_pair_valid(p, output ? mOutputs : mInputs)) {
            *status = kAudioHardwareIllegalOperationError;
            return true;
        }
        *status = kAudioHardwareNoError;
        uint32_t packed = ozzy_stereo_pair_pack(p);
        if ((output ? mPairOut : mPairIn).exchange(packed) == packed) return true;
        Store(host, output ? kOutPairKey : kInPairKey, Number((SInt32)packed));
        Notify(host, device, kAudioDevicePropertyPreferredChannelsForStereo,
               output ? kAudioObjectPropertyScopeOutput : kAudioObjectPropertyScopeInput);
        return true;
    }

    // --- The volume and mute control objects (OzzyIsExtraControl kinds) ---

    static bool HasControlProperty(uint32_t kind, const AudioObjectPropertyAddress* a) {
        switch (a->mSelector) {
            case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyName:
            case kAudioObjectPropertyOwner: case kAudioObjectPropertyOwnedObjects:
            case kAudioControlPropertyScope: case kAudioControlPropertyElement: return true;
            case kAudioLevelControlPropertyScalarValue: case kAudioLevelControlPropertyDecibelValue:
            case kAudioLevelControlPropertyDecibelRange: case kAudioLevelControlPropertyConvertScalarToDecibels:
            case kAudioLevelControlPropertyConvertDecibelsToScalar: return kind == kOzzyObjectVolumeControl;
            case kAudioBooleanControlPropertyValue: return kind == kOzzyObjectMuteControl;
        }
        return false;
    }

    static bool IsControlPropertySettable(uint32_t kind, const AudioObjectPropertyAddress* a) {
        if (kind == kOzzyObjectVolumeControl)
            return a->mSelector == kAudioLevelControlPropertyScalarValue || a->mSelector == kAudioLevelControlPropertyDecibelValue;
        return kind == kOzzyObjectMuteControl && a->mSelector == kAudioBooleanControlPropertyValue;
    }

    static OSStatus GetControlPropertyDataSize(uint32_t kind, const AudioObjectPropertyAddress* a, UInt32* size) {
        if (!HasControlProperty(kind, a)) return kAudioHardwareUnknownPropertyError;
        switch (a->mSelector) {
            case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *size = sizeof(AudioClassID); break;
            case kAudioObjectPropertyName: *size = sizeof(CFStringRef); break;
            case kAudioObjectPropertyOwner: *size = sizeof(AudioObjectID); break;
            case kAudioObjectPropertyOwnedObjects: *size = 0; break;
            case kAudioLevelControlPropertyDecibelRange: *size = sizeof(AudioValueRange); break;
            case kAudioLevelControlPropertyScalarValue: case kAudioLevelControlPropertyDecibelValue:
            case kAudioLevelControlPropertyConvertScalarToDecibels:
            case kAudioLevelControlPropertyConvertDecibelsToScalar: *size = sizeof(Float32); break;
            default: *size = sizeof(UInt32); break;   // scope, element, the mute value
        }
        return kAudioHardwareNoError;
    }

    OSStatus GetControlPropertyData(AudioObjectID device, uint32_t kind, const AudioObjectPropertyAddress* a,
                                    UInt32 inMax, UInt32* outSize, void* outData) const {
        UInt32 size = 0;
        OSStatus st = GetControlPropertyDataSize(kind, a, &size);
        if (st != kAudioHardwareNoError) return st;
        if (outData && inMax < size) return kAudioHardwareBadPropertySizeError;
        if (outSize) *outSize = size;
        if (!outData) return kAudioHardwareNoError;
        bool volume = kind == kOzzyObjectVolumeControl;
        switch (a->mSelector) {
            case kAudioObjectPropertyBaseClass:
                *(AudioClassID*)outData = volume ? kAudioLevelControlClassID : kAudioBooleanControlClassID; break;
            case kAudioObjectPropertyClass:
                *(AudioClassID*)outData = volume ? kAudioVolumeControlClassID : kAudioMuteControlClassID; break;
            case kAudioObjectPropertyName:
                *(CFStringRef*)outData = volume ? CFSTR("Output Volume") : CFSTR("Output Mute"); break;
            case kAudioObjectPropertyOwner: *(AudioObjectID*)outData = device; break;
            case kAudioControlPropertyScope: *(UInt32*)outData = kAudioObjectPropertyScopeOutput; break;
            case kAudioControlPropertyElement: *(UInt32*)outData = kAudioObjectPropertyElementMain; break;
            case kAudioLevelControlPropertyScalarValue: *(Float32*)outData = mVolume.load(); break;
            case kAudioLevelControlPropertyDecibelValue: *(Float32*)outData = ozzy_volume_scalar_to_db(mVolume.load()); break;
            case kAudioLevelControlPropertyDecibelRange:
                *(AudioValueRange*)outData = AudioValueRange { ozzy_volume_min_db, ozzy_volume_max_db }; break;
            // The value to convert comes in outData and is converted in place
            case kAudioLevelControlPropertyConvertScalarToDecibels:
                *(Float32*)outData = ozzy_volume_scalar_to_db(*(Float32*)outData); break;
            case kAudioLevelControlPropertyConvertDecibelsToScalar:
                *(Float32*)outData = ozzy_volume_db_to_scalar(*(Float32*)outData); break;
            case kAudioBooleanControlPropertyValue: *(UInt32*)outData = mMuted.load() ? 1 : 0; break;
        }
        return kAudioHardwareNoError;
    }

    OSStatus SetControlPropertyData(AudioServerPlugInHostRef host, AudioObjectID device, uint32_t kind,
                                    const AudioObjectPropertyAddress* a, UInt32 size, const void* data) {
        if (!IsControlPropertySettable(kind, a)) return kAudioHardwareUnsupportedOperationError;
        if (!data || size < (kind == kOzzyObjectVolumeControl ? sizeof(Float32) : sizeof(UInt32)))
            return kAudioHardwareBadPropertySizeError;
        AudioObjectID control = device + kind;
        if (kind == kOzzyObjectVolumeControl) {
            Float32 v = *(const Float32*)data;
            if (a->mSelector == kAudioLevelControlPropertyDecibelValue) v = ozzy_volume_db_to_scalar(v);
            v = ozzy_volume_clamp(v);
            if (mVolume.exchange(v) == v) return kAudioHardwareNoError;
            Store(host, kVolumeKey, CFNumberCreate(NULL, kCFNumberFloat32Type, &v));
            Notify(host, control, kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal);
            Notify(host, control, kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal);
        } else {
            bool muted = *(const UInt32*)data != 0;
            if (mMuted.exchange(muted) == muted) return kAudioHardwareNoError;
            Store(host, kMuteKey, CFRetain(muted ? kCFBooleanTrue : kCFBooleanFalse));
            Notify(host, control, kAudioBooleanControlPropertyValue, kAudioObjectPropertyScopeGlobal);
        }
        return kAudioHardwareNoError;
    }

private:
    static constexpr uint32_t kDefaultPair = 1u << 16 | 2u;   // ozzy_stereo_pair_pack({ 1, 2 })
    static constexpr const char* kVolumeKey = "Output Volume";
    static constexpr const char* kMuteKey = "Output Mute";
    static constexpr const char* kInPairKey = "Input Stereo Pair";
    static constexpr const char* kOutPairKey = "Output Stereo Pair";
    // Ozzy 0.3.0's keys, one per plug-in (it drove one device): read for a device that has none of
    // its own yet
    static constexpr const char* kLegacyVolumeKey = "output volume";
    static constexpr const char* kLegacyMuteKey = "output mute";

    AudioObjectID mDevice = kAudioObjectUnknown;
    const struct ozzy_device* mModel = nullptr;
    uint32_t mInputs = 0, mOutputs = 0;
    const struct ozzy_mono_input* mMono[OZZY_MAX_MONO_INPUTS] = {};
    unsigned int mMonoCount = 0;
    CFStringRef mUID = nullptr;
    std::atomic<float> mVolume { 1.0f };
    std::atomic<bool> mMuted { false };
    float mAppliedGain = 1.0f;   // the IO thread's, where its last ramp ended
    std::atomic<uint32_t> mPairIn { kDefaultPair }, mPairOut { kDefaultPair };

    static bool IsOutput(AudioObjectPropertyScope scope) {
        return scope == kAudioObjectPropertyScopeOutput || scope == kAudioObjectPropertyScopeGlobal;
    }

    const char* ChannelName(const AudioObjectPropertyAddress* a) const {
        if (a->mScope != kAudioObjectPropertyScopeInput && a->mScope != kAudioObjectPropertyScopeOutput) return nullptr;
        bool output = a->mScope == kAudioObjectPropertyScopeOutput;
        if (a->mElement > (output ? mOutputs : mInputs)) return nullptr;
        const struct ozzy_channel_info* info = ozzy_channel(mModel, output, a->mElement);
        return info ? info->name : nullptr;
    }

    struct ozzy_stereo_pair Pair(AudioObjectPropertyScope scope) const {
        bool output = IsOutput(scope);
        return ozzy_stereo_pair_for(ozzy_stereo_pair_unpack((output ? mPairOut : mPairIn).load()), output ? mOutputs : mInputs);
    }

    /* The storage key "<device UID> <what>", like the settings' */
    CFStringRef CopyKey(const char* what) const {
        return mUID ? CFStringCreateWithFormat(NULL, NULL, CFSTR("%@ %s"), mUID, what) : nullptr;
    }

    CFPropertyListRef Copy(AudioServerPlugInHostRef host, const char* what) const {
        CFStringRef key = CopyKey(what);
        CFPropertyListRef v = nullptr;
        if (host && key && host->CopyFromStorage(host, key, &v) != kAudioHardwareNoError) v = nullptr;
        if (key) CFRelease(key);
        return v;
    }

    /* What storage has under @what, or else under the plug-in-wide @legacy key, which is then
     * stored under @what so the device has its own from now on */
    CFPropertyListRef CopyOrMigrate(AudioServerPlugInHostRef host, const char* what, const char* legacy) const {
        CFPropertyListRef v = Copy(host, what);
        if (v || !host) return v;
        CFStringRef key = CFStringCreateWithCString(NULL, legacy, kCFStringEncodingUTF8);
        if (key && host->CopyFromStorage(host, key, &v) != kAudioHardwareNoError) v = nullptr;
        if (key) CFRelease(key);
        if (v) Store(host, what, CFRetain(v));
        return v;
    }

    /* Write @value (released here) under @what */
    void Store(AudioServerPlugInHostRef host, const char* what, CFPropertyListRef value) const {
        CFStringRef key = CopyKey(what);
        if (host && key && value) host->WriteToStorage(host, key, value);
        if (key) CFRelease(key);
        if (value) CFRelease(value);
    }

    static CFNumberRef Number(SInt32 n) { return CFNumberCreate(NULL, kCFNumberSInt32Type, &n); }

    static void Notify(AudioServerPlugInHostRef host, AudioObjectID object, AudioObjectPropertySelector selector,
                       AudioObjectPropertyScope scope) {
        if (!host) return;
        AudioObjectPropertyAddress addr = { selector, scope, kAudioObjectPropertyElementMain };
        host->PropertiesChanged(host, object, 1, &addr);
    }
};

#endif
