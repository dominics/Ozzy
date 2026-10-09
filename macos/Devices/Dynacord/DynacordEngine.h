/* SPDX-License-Identifier: MIT */
#pragma once
#include "../../OzzyCore/OzzyEngine.h"
#include "../../Shared/OzzySharedData.h"
#include "../../../common/devices/ploytec/dynacord_pacer.h"
#include "DynacordRing.h"

/*
 * Dynacord CMS 600-3 (old Ploytec card, 0562:03eb): iso S24_3LE out on 0x02
 * paced by explicit feedback on 0x81, bulk 64-byte Ploytec frames in on
 * 0x86. Built into the kext (KextBus) and the userspace daemon (UserBus).
 */
class DynacordEngine : public OzzyEngine {
public:
	enum : uint32_t { kMaxOutXfers = 32 };

	/* outXfers: iso OUT transfers of 3 ms kept queued, clamped to 1..kMaxOutXfers. */
	explicit DynacordEngine(uint32_t outXfers);
	uint32_t OutXfers() const { return mOutXfers; }

	/* Rate for the next Start(): 44100, 48000 or 96000. */
	void SetRate(uint32_t rate) override { mRate = rate; }
	uint32_t Rate() const { return mRate; }
	uint32_t NextRate() const override { return mNextRate; }
	static bool IsSupportedRate(uint32_t rate) { return rate == 44100 || rate == 48000 || rate == 96000; }

	uint32_t LayoutVersion() const override { return kOzzyLayoutFrames; }
	const OzzyDeviceProfile& GetProfile() const override { return mProfile; }
	void SetBulk(bool) override {}

	/* Run the rate init sequence, publish the ring and timing config, then
	 * start streaming. False if the device didn't accept the rate. */
	bool Start() override;

	/* Stop resubmitting; completions after this are ignored. */
	void Stop() override;

	void OnPacketComplete(uint8_t pipeID, void* ctx, int status, uint32_t bytes) override;

private:
	bool RunInit();
	bool Control(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, uint8_t* data, uint16_t len, const char* what);
	void PublishConfig();
	volatile OzzySharedMemory* SHM() { return (volatile OzzySharedMemory*)mSharedMemory; }

	enum : uint32_t { kSafetyInMs = 25 };

	OzzyDeviceProfile mProfile;
	uint32_t mOutXfers;
	uint32_t mRate = 48000;
	uint32_t mNextRate = 0;
	DynacordRingLayout mLayout {};
	bool mStopping = false;
};
