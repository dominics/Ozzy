/* SPDX-License-Identifier: MIT */
#include "DynacordEngine.h"
#include "../../Shared/OzzyLog.h"
#include "../../../common/devices/ploytec/ploytec_protocol.h"

DynacordEngine::DynacordEngine(uint32_t outXfers)
	: mOutXfers(outXfers < 1 ? 1 : outXfers > kMaxOutXfers ? kMaxOutXfers : outXfers)
{
	mProfile.interfaceCount = 2;
	mProfile.interfaceIndices[0] = 0;
	mProfile.interfaceIndices[1] = 1;
	mProfile.altSettings[0] = 1;
	mProfile.altSettings[1] = 1;
	mProfile.isoOut = { DYNACORD_EP_PCM_OUT, 0 };
	mProfile.feedbackIn = { DYNACORD_EP_FEEDBACK, 1 };
	mProfile.pcmIn = { DYNACORD_EP_PCM_IN, 1 };
	mProfile.midiOut = { DYNACORD_EP_MIDI_OUT, 0 };
	mProfile.midiIn = { DYNACORD_EP_MIDI_IN, 0 };
}

bool DynacordEngine::Control(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, uint8_t* data, uint16_t len,
			     const char* what)
{
	if (mBus->VendorRequest(type, req, val, idx, data, len))
		return true;
	LogDynacordError("%{public}s (%02x %02x %04x %04x) failed", what, type, req, val, idx);
	return false;
}

/* The Ploytec rate sequence as the Windows driver sends it to the CMS
 * (FINDINGS.md "Control sequence"; same as ploytec-play's init.c). */
bool DynacordEngine::RunInit()
{
	uint8_t buf[16] = {}, rbuf[3], status = 0;

	if (!Control(0xC0, PLOYTEC_CMD_FIRMWARE, 0, 0, buf, 15, "firmware"))
		return false;
	struct ploytec_firmware_version fw = ploytec_parse_firmware(buf);
	LogDynacord("firmware: chip %02x, version %u.%u", fw.chip_id, fw.major, fw.minor);

	if (!Control(0xC0, PLOYTEC_CMD_STATUS, 0, 0, &status, 1, "status"))
		return false;
	if (!Control(PLOYTEC_CMD_GET_RATE_TYPE, PLOYTEC_CMD_GET_RATE_REQ, 0x0100, 0, rbuf, 3, "get rate"))
		return false;
	LogDynacord("current rate: %u Hz", ploytec_decode_rate(rbuf));

	ploytec_encode_rate(mRate, rbuf);
	for (int i = 0; i < 5; i++) {
		uint16_t ep = i % 2 == 0 ? PLOYTEC_EP_RATE_IN : DYNACORD_EP_RATE_OUT;
		if (!Control(PLOYTEC_CMD_SET_RATE_TYPE, PLOYTEC_CMD_SET_RATE_REQ, 0x0100, ep, rbuf, 3, "set rate"))
			return false;
		mBus->Sleep(10);
	}

	if (!Control(PLOYTEC_CMD_GET_RATE_TYPE, PLOYTEC_CMD_GET_RATE_REQ, 0x0100, PLOYTEC_EP_RATE_IN, rbuf, 3,
		     "verify rate"))
		return false;
	if (ploytec_decode_rate(rbuf) != mRate) {
		LogDynacordError("device reports %u Hz after setting %u Hz", ploytec_decode_rate(rbuf), mRate);
		return false;
	}

	if (!Control(0xC0, PLOYTEC_CMD_STATUS, 0, 0, &status, 1, "status"))
		return false;
	return Control(0x40, PLOYTEC_CMD_STATUS, ploytec_confirm_wvalue(status), 0, nullptr, 0, "confirm");
}

/* Ring and timing description the HAL reads from a v2 segment. Output
 * safety: the OUT queue (3 ms per transfer) + one transfer + 13 ms for HAL
 * IO-thread jitter. Input: one 3 ms transfer + 4.5 ms worst completion gap,
 * with room for the not-yet-measured input lead: 25 ms. Both are starting
 * values; Task 10's probe measures the real leads. */
void DynacordEngine::PublishConfig()
{
	volatile OzzyAudioBuffer& a = SHM()->audio;
	mLayout = DynacordRingLayoutFor(mRate);
	a.outputBytesPerFrame = kDynacordOutFrameBytes;
	a.inputBytesPerFrame = kDynacordInFrameBytes;
	a.framesPerPacket = mLayout.inFramesPerXfer;
	a.deviceFlags = 0;
	a.ringFramesOut = mLayout.outFrames;
	a.ringFramesIn = mLayout.inFrames;
	a.zeroTimestampPeriod = mRate / 10;
	a.safetyOffsetOut = mRate * (mOutXfers * 3 + 16) / 1000;
	a.safetyOffsetIn = mRate * kSafetyInMs / 1000;
	a.sampleRate = mRate;
	a.requestedRate = 0;
}

bool DynacordEngine::Start()
{
	if (!mBus || !mSharedMemory || !IsSupportedRate(mRate))
		return false;
	mStopping = false;
	if (!RunInit())
		return false;
	PublishConfig();
	SHM()->audio.hardwarePresent = true;
	return true;
}

void DynacordEngine::Stop()
{
	mStopping = true;
}

void DynacordEngine::OnPacketComplete(uint8_t, void*, int, uint32_t) {}
