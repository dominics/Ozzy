/* SPDX-License-Identifier: MIT */
/*
 * Milestone 0: publish a Dynacord-shaped v2 segment with a free-running
 * 48 kHz clock and no USB, to prove coreaudiod's sandboxed OzzyHAL can map
 * a segment created by a non-root process. Prints once a second whether
 * the HAL is writing non-silent output.
 */
#include <mach/mach_time.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include "../SharedMemory.h"
#include "../../../Devices/Dynacord/DynacordRing.h"
#include "../../../../common/devices/ploytec/ploytec_defs.h"

static volatile sig_atomic_t gStop;
static void OnSignal(int) { gStop = 1; }

int main()
{
	signal(SIGINT, OnSignal);
	signal(SIGTERM, OnSignal);
	SharedMemorySegment seg;
	SharedMemorySegment::CreateResult r = seg.Create();
	if (r == SharedMemorySegment::CreateResult::kInUse || r == SharedMemorySegment::CreateResult::kError) {
		fprintf(stderr, "fake-daemon: cannot create segment: %s\n", seg.Reason());
		return 1;
	}
	OzzySharedMemory* shm = seg.Get();
	/* CoreAudio needs a zero timestamp period at least as long as its IO buffer. */
	const uint32_t rate = 48000, period = rate / 10;
	DynacordRingLayout l = DynacordRingLayoutFor(rate);
	seg.PublishIdentity(DYNACORD_VENDOR_ID, DYNACORD_PID_CMS600_3, "DYNACORD", "CMS 600-3 (fake)", "FAKE0001");
	shm->audio.sampleRate = rate;
	shm->audio.outputBytesPerFrame = kDynacordOutFrameBytes;
	shm->audio.inputBytesPerFrame = kDynacordInFrameBytes;
	shm->audio.ringFramesOut = l.outFrames;
	shm->audio.ringFramesIn = l.inFrames;
	shm->audio.zeroTimestampPeriod = period;
	shm->audio.safetyOffsetOut = rate * 40 / 1000;
	shm->audio.safetyOffsetIn = rate * 25 / 1000;
	seg.NewSession();
	shm->audio.hardwarePresent = true;

	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	const uint64_t start = mach_absolute_time();
	const double ticksPerFrame = 1e9 / rate * tb.denom / tb.numer;
	uint64_t nextAnchor = 0, consumed = 0, nonzero = 0, lastPrint = 0, lastTick = 0;
	while (!gStop) {
		usleep(1000);
		uint64_t now = mach_absolute_time();
		uint64_t due = (uint64_t)((now - start) / ticksPerFrame);
		for (; consumed < due; consumed++) {
			uint8_t frame[kDynacordOutFrameBytes];
			DynacordTakeOutput(shm->audio.outputBuffer, l.outFrames, consumed, frame, 1);
			for (uint8_t b : frame)
				nonzero += b != 0;
		}
		while (nextAnchor <= consumed) {
			auto& ts = shm->audio.timestamp;
			uint32_t seq = ts.sequence.load();
			ts.sequence = seq + 1;
			ts.sampleTime = nextAnchor;
			ts.hostTime = start + (uint64_t)(nextAnchor * ticksPerFrame);
			ts.sequence = seq + 2;
			nextAnchor += period;
			shm->audio.driverReady = true;
		}
		if (consumed / rate != lastPrint) {
			lastPrint = consumed / rate;
			seg.TickHeartbeat();
			printf("t=%llus output %s\n", (unsigned long long)lastPrint, nonzero ? "ACTIVE" : "silent");
			fflush(stdout);
			nonzero = 0;
		}
		if (consumed / (rate / 10) != lastTick) {
			lastTick = consumed / (rate / 10);
			seg.TickHeartbeat();
		}
	}
	seg.Destroy();
	return 0;
}
