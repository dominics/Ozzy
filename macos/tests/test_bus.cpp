/* SPDX-License-Identifier: MIT */
#include "FakeBus.h"
#include "test.h"
#include "tests.h"

void test_bus_isoch_defaults_unsupported()
{
	MinimalBus bus;
	OzzyIsochFrame f[1] = { { 12, 0, 0 } };
	uint8_t buf[12];
	uint64_t frame = 0, host = 0;
	CHECK(!bus.SubmitIsochPacket(0x02, buf, f, 1, nullptr));
	CHECK(!bus.GetBusFrame(&frame, &host));
}

void test_bus_profile_defaults_zero()
{
	OzzyDeviceProfile p;
	CHECK_EQ(p.altSettings[0], 0);
	CHECK_EQ(p.altSettings[3], 0);
	CHECK_EQ(p.midiOut.address, 0);
	CHECK_EQ(p.isoOut.address, 0);
	CHECK_EQ(p.feedbackIn.address, 0);
}

namespace {
struct NullEngine : OzzyEngine {
	const OzzyDeviceProfile& GetProfile() const override { return p; }
	void SetBulk(bool) override {}
	bool Start() override { return true; }
	void Stop() override {}
	void OnPacketComplete(uint8_t, void*, int, uint32_t) override {}
	OzzyDeviceProfile p;
};
}

void test_engine_base_defaults()
{
	NullEngine e;
	CHECK_EQ(e.LayoutVersion(), (uint32_t)kOzzyLayoutPackets);
	CHECK(e.Poll() == OzzyEngine::PollResult::kRunning);
	CHECK_EQ(e.NextRate(), 0);
}
