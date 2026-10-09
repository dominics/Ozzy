/* SPDX-License-Identifier: MIT */
#include <memory>
#include "../Devices/Ploytec/PloytecEngine.h"
#include "../Shared/OzzySharedData.h"
#include "FakeBus.h"
#include "test.h"
#include "tests.h"

static const int kAborted = (int)0xE00002EB;         /* kIOReturnAborted */
static const int kNotResponding = (int)0xE00002ED;   /* kIOReturnNotResponding */

/* A started Xone engine on a FakeBus, its six initial transfers queued. */
struct XoneFixture {
	FakeBus bus;
	std::unique_ptr<OzzySharedMemory> shm { new OzzySharedMemory() };
	PloytecEngine engine { 0x0001 };

	XoneFixture()
	{
		engine.Init(&bus, shm.get());
		engine.SetBulk(true);
		bus.SetEngine(&engine);
	}
};

/* The kext aborts every transfer after Stop() and waits for the drain: an
 * aborted completion must not queue another transfer. */
void test_ploytec_stop_stops_resubmitting()
{
	XoneFixture f;
	CHECK(f.engine.Start());
	size_t queued = f.bus.bulk.size();
	CHECK(queued > 0);
	f.engine.Stop();
	for (int i = 0; i < 100 && !f.bus.bulk.empty(); i++) {
		FakeBus::Bulk b = f.bus.bulk.front();
		f.bus.bulk.pop_front();
		f.engine.OnPacketComplete(b.pipe, b.ctx, kAborted, 0);
	}
	CHECK_EQ(f.bus.bulk.size(), 0u);
	CHECK(f.engine.Start());   /* a restart resubmits again */
	CHECK_EQ(f.bus.bulk.size(), queued);
}

/* An aborted transfer is not resubmitted; any other error is, as is a good
 * transfer, since nothing would restart a stalled Xone pipe. */
void test_ploytec_only_aborted_transfer_not_resubmitted()
{
	XoneFixture f;
	CHECK(f.engine.Start());
	size_t queued = f.bus.bulk.size();
	FakeBus::Bulk b = f.bus.bulk.front();
	f.bus.bulk.pop_front();
	f.engine.OnPacketComplete(b.pipe, b.ctx, kAborted, 0);
	CHECK_EQ(f.bus.bulk.size(), queued - 1);
	b = f.bus.bulk.front();
	f.bus.bulk.pop_front();
	f.engine.OnPacketComplete(b.pipe, b.ctx, kNotResponding, 0);
	CHECK_EQ(f.bus.bulk.size(), queued - 1);
	b = f.bus.bulk.front();
	f.bus.bulk.pop_front();
	f.engine.OnPacketComplete(b.pipe, b.ctx, 0, 0);
	CHECK_EQ(f.bus.bulk.size(), queued - 1);
}
