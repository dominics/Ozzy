/* SPDX-License-Identifier: MIT */
#include "../OzzyCore/OzzyFactory.h"
#include "engine_fixture.h"
#include "test.h"
#include "tests.h"

void test_engine_profile()
{
	DynacordEngine e(8);
	const OzzyDeviceProfile& p = e.GetProfile();
	CHECK_EQ(e.LayoutVersion(), (uint32_t)kOzzyLayoutFrames);
	CHECK_EQ(p.interfaceCount, 2);
	CHECK_EQ(p.interfaceIndices[0], 0);
	CHECK_EQ(p.interfaceIndices[1], 1);
	CHECK_EQ(p.altSettings[0], 1);
	CHECK_EQ(p.altSettings[1], 1);
	CHECK_EQ(p.isoOut.address, DYNACORD_EP_PCM_OUT);
	CHECK_EQ(p.isoOut.interfaceIndex, 0);
	CHECK_EQ(p.feedbackIn.address, DYNACORD_EP_FEEDBACK);
	CHECK_EQ(p.feedbackIn.interfaceIndex, 1);
	CHECK_EQ(p.pcmIn.address, DYNACORD_EP_PCM_IN);
	CHECK_EQ(p.pcmIn.interfaceIndex, 1);
	CHECK_EQ(p.midiOut.address, DYNACORD_EP_MIDI_OUT);
	CHECK_EQ(p.midiOut.interfaceIndex, 0);
	CHECK_EQ(p.midiIn.address, DYNACORD_EP_MIDI_IN);
	CHECK_EQ(p.midiIn.interfaceIndex, 0);
}

void test_engine_out_xfers_clamped()
{
	CHECK_EQ(DynacordEngine(0).OutXfers(), 1);
	CHECK_EQ(DynacordEngine(8).OutXfers(), 8);
	CHECK_EQ(DynacordEngine(99).OutXfers(), DynacordEngine::kMaxOutXfers);
}

void test_engine_factory()
{
	OzzyEngine* e = OzzyFactory::CreateEngine(DYNACORD_VENDOR_ID, DYNACORD_PID_CMS600_3);
	CHECK(dynamic_cast<DynacordEngine*>(e) != nullptr);
	delete e;

	OzzyEngine* x = OzzyFactory::CreateEngine(PLOYTEC_VENDOR_ID, 0x0001);
	CHECK(x != nullptr && dynamic_cast<DynacordEngine*>(x) == nullptr);
	if (x) {
		const OzzyDeviceProfile& xp = x->GetProfile();
		CHECK_EQ(xp.altSettings[0], 1);
		CHECK_EQ(xp.altSettings[1], 1);
		CHECK_EQ(xp.pcmOut.address, 0x05);
		CHECK_EQ(xp.pcmIn.address, 0x86);
		CHECK_EQ(xp.midiIn.address, 0x83);
	}
	delete x;
}

/* The same sequence as userspace/ploytec-play/init.c, which matches the
 * Windows driver's captures (FINDINGS.md, "Control sequence"). */
void test_engine_init_sequence()
{
	EngineFixture f(96000);
	CHECK(f.engine.Start());
	const auto& c = f.bus.controls;
	CHECK_EQ(c.size(), 11);
	if (c.size() != 11)
		return;
	CHECK(c[0].type == 0xC0 && c[0].req == PLOYTEC_CMD_FIRMWARE && c[0].len == 15);
	CHECK(c[1].type == 0xC0 && c[1].req == PLOYTEC_CMD_STATUS && c[1].len == 1);
	CHECK(c[2].type == PLOYTEC_CMD_GET_RATE_TYPE && c[2].req == PLOYTEC_CMD_GET_RATE_REQ && c[2].val == 0x0100 && c[2].idx == 0);
	for (int i = 0; i < 5; i++) {
		const auto& s = c[3 + i];
		CHECK(s.type == PLOYTEC_CMD_SET_RATE_TYPE && s.req == PLOYTEC_CMD_SET_RATE_REQ && s.val == 0x0100 && s.len == 3);
		CHECK_EQ(s.idx, i % 2 == 0 ? PLOYTEC_EP_RATE_IN : DYNACORD_EP_RATE_OUT);
		CHECK_EQ(ploytec_decode_rate(s.out.data()), 96000);
	}
	CHECK(c[8].type == PLOYTEC_CMD_GET_RATE_TYPE && c[8].idx == PLOYTEC_EP_RATE_IN);
	CHECK(c[9].type == 0xC0 && c[9].req == PLOYTEC_CMD_STATUS);
	CHECK(c[10].type == 0x40 && c[10].req == PLOYTEC_CMD_STATUS && c[10].val == ploytec_confirm_wvalue(0x32) && c[10].len == 0);
}

void test_engine_init_rate_mismatch_fails()
{
	EngineFixture f(96000);
	f.bus.responder = [](const FakeBus::Control& c, uint8_t* data) {
		if (c.type == PLOYTEC_CMD_GET_RATE_TYPE)
			ploytec_encode_rate(44100, data);   /* device ignores the change */
		else if (c.type & 0x80)
			std::memset(data, 0x32, c.len);
		return true;
	};
	CHECK(!f.engine.Start());
	CHECK(!f.shm->audio.hardwarePresent);
}

void test_engine_init_control_failure_fails()
{
	EngineFixture f;
	f.bus.responder = [](const FakeBus::Control&, uint8_t*) { return false; };
	CHECK(!f.engine.Start());
}

void test_engine_publishes_config()
{
	EngineFixture f(44100);
	CHECK(f.engine.Start());
	const auto& a = f.shm->audio;
	DynacordRingLayout l = DynacordRingLayoutFor(44100);
	CHECK_EQ(a.sampleRate, 44100);
	CHECK_EQ(a.outputBytesPerFrame, kDynacordOutFrameBytes);
	CHECK_EQ(a.inputBytesPerFrame, kDynacordInFrameBytes);
	CHECK_EQ(a.ringFramesOut, l.outFrames);
	CHECK_EQ(a.ringFramesIn, l.inFrames);
	CHECK_EQ(a.zeroTimestampPeriod, 4410);
	CHECK_EQ(a.safetyOffsetOut, 44100 * 40 / 1000);
	CHECK_EQ(a.safetyOffsetIn, 44100 * 25 / 1000);
	CHECK(a.hardwarePresent);
	CHECK_EQ(a.requestedRate, 0);
}

void test_engine_safety_follows_queue()
{
	EngineFixture f(48000);
	DynacordEngine deep(32);
	deep.SetRate(48000);
	deep.Init(&f.bus, f.shm.get());
	CHECK(deep.Start());
	CHECK_EQ(f.shm->audio.safetyOffsetOut, 48000 * (32 * 3 + 16) / 1000);
}
