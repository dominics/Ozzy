/* SPDX-License-Identifier: MIT */
/*
 * Exercise OzzyDaemon without CoreAudio: map the segment like OzzyHAL does,
 * estimate the playhead from the latest anchor, keep a sine tone written
 * safetyOffsetOut + 10 ms ahead of it, and print per-channel input peaks.
 *
 * The segment is world-writable, so every field that sizes or indexes a ring
 * is read once into a local and checked before use.
 */
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../../../common/devices/ploytec/ploytec_defs.h"
#include "../../../Devices/Dynacord/DynacordCodec.h"
#include "../../../Devices/Dynacord/DynacordRing.h"
#include "../../../Shared/OzzySharedData.h"
#include "../RealTime.h"

static const int kReadyTimeoutSeconds = 10;
static const long kMaxSeconds = 86400;

static void Usage(FILE* out)
{
	fprintf(out, "usage: shm-client [--tone HZ] [--channel 1-4|all] [--seconds N]\n");
}

/* Parse a whole decimal string into a long; false on junk, overflow or trailing text. */
static bool ParseLong(const char* s, long& out)
{
	char* end = nullptr;
	errno = 0;
	long v = strtol(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0')
		return false;
	out = v;
	return true;
}

static double TicksPerSecond()
{
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	return 1e9 * tb.denom / tb.numer;
}

/* Print the refused field and value, then exit 1. */
[[noreturn]] static void Refuse(const char* field, uint64_t value, const char* expected)
{
	fprintf(stderr, "refusing segment: %s = %llu (expected %s)\n", field, (unsigned long long)value, expected);
	exit(1);
}

int main(int argc, char** argv)
{
	double hz = 440;
	int channel = -1, seconds = 10;
	for (int i = 1; i < argc; i += 2) {
		if (i + 1 >= argc) {
			fprintf(stderr, "missing value for %s\n", argv[i]);
			Usage(stderr);
			return 2;
		}
		if (!strcmp(argv[i], "--tone")) {
			char* end = nullptr;
			hz = strtod(argv[i + 1], &end);
			if (end == argv[i + 1] || *end != '\0' || !(hz > 0) || !isfinite(hz)) {
				fprintf(stderr, "--tone must be a positive number, got '%s'\n", argv[i + 1]);
				return 2;
			}
		} else if (!strcmp(argv[i], "--channel")) {
			if (!strcmp(argv[i + 1], "all")) {
				channel = -1;
			} else {
				long c;
				if (!ParseLong(argv[i + 1], c) || c < 1 || c > 4) {
					fprintf(stderr, "--channel must be 1-4 or all, got '%s'\n", argv[i + 1]);
					return 2;
				}
				channel = (int)c - 1;
			}
		} else if (!strcmp(argv[i], "--seconds")) {
			long s;
			if (!ParseLong(argv[i + 1], s) || s < 1 || s > kMaxSeconds) {
				fprintf(stderr, "--seconds must be 1-%ld, got '%s'\n", kMaxSeconds, argv[i + 1]);
				return 2;
			}
			seconds = (int)s;
		} else {
			fprintf(stderr, "unknown argument '%s'\n", argv[i]);
			Usage(stderr);
			return 2;
		}
	}

	int fd = shm_open(kOzzySharedMemName, O_RDWR, 0);
	if (fd < 0) { perror("shm_open (is OzzyDaemon running?)"); return 1; }
	struct stat st;
	if (fstat(fd, &st) != 0) { perror("fstat"); return 1; }
	if ((unsigned long long)st.st_size < sizeof(OzzySharedMemory)) {
		fprintf(stderr, "segment too small: %lld bytes, need %zu\n", (long long)st.st_size, sizeof(OzzySharedMemory));
		return 1;
	}
	OzzySharedMemory* shm = (OzzySharedMemory*)mmap(nullptr, sizeof(OzzySharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (shm == MAP_FAILED) { perror("mmap"); return 1; }

	if (shm->magic != kOzzyMagic) {
		fprintf(stderr, "not an Ozzy segment: magic = 0x%08x\n", shm->magic);
		return 1;
	}
	int waited = 0;
	while (!shm->audio.driverReady.load()) {
		if (shm->magic != kOzzyMagic) {
			fprintf(stderr, "segment magic cleared while waiting; daemon gone\n");
			return 1;
		}
		if (waited++ >= kReadyTimeoutSeconds * 100) {
			fprintf(stderr, "daemon not ready after %d s\n", kReadyTimeoutSeconds);
			return 1;
		}
		usleep(10000);
	}

	const uint32_t magic = shm->magic;
	const uint32_t version = shm->version;
	const uint16_t vendor = shm->vendorID;
	const uint32_t rate = shm->audio.sampleRate.load();
	const uint32_t outBytes = shm->audio.outputBytesPerFrame;
	const uint32_t inBytes = shm->audio.inputBytesPerFrame;
	const uint32_t ringOut = shm->audio.ringFramesOut;
	const uint32_t ringIn = shm->audio.ringFramesIn;
	const uint32_t safetyOut = shm->audio.safetyOffsetOut;
	const uint32_t safetyIn = shm->audio.safetyOffsetIn;

	if (magic != kOzzyMagic)
		Refuse("magic", magic, "kOzzyMagic");
	if (version != kOzzyLayoutFrames)
		Refuse("version", version, "2");
	if (vendor != DYNACORD_VENDOR_ID)
		Refuse("vendorID", vendor, "0x0562");
	if (rate != 44100 && rate != 48000 && rate != 96000)
		Refuse("sampleRate", rate, "44100, 48000 or 96000");
	if (outBytes != kDynacordOutFrameBytes)
		Refuse("outputBytesPerFrame", outBytes, "12");
	if (inBytes != kDynacordInFrameBytes)
		Refuse("inputBytesPerFrame", inBytes, "64");
	const uint64_t maxOutFrames = sizeof(shm->audio.outputBuffer) / kDynacordOutFrameBytes;
	const uint64_t maxInFrames = sizeof(shm->audio.inputBuffer) / kDynacordInFrameBytes;
	if (ringOut < 1 || ringOut > maxOutFrames)
		Refuse("ringFramesOut", ringOut, "1..maxOutFrames");
	if (ringIn < 1 || ringIn > maxInFrames)
		Refuse("ringFramesIn", ringIn, "1..maxInFrames");
	if ((uint64_t)safetyOut + rate / 100 >= ringOut)
		Refuse("safetyOffsetOut", safetyOut, "less than ringFramesOut - rate/100");

	/* Stand in for coreaudiod's real-time IO thread. A normal-priority thread
	 * can wake 20 ms late, which pushes the write position past a stretch the
	 * daemon has not played yet, and the stale ring data there plays as a pop. */
	char rtErr[256];
	if (!SetRealTimePolicy(rtErr, sizeof(rtErr)))
		fprintf(stderr, "%s; expect pops\n", rtErr);

	const double tps = TicksPerSecond();
	uint64_t written = 0;
	double phase = 0;
	float peak[4] = {};
	uint64_t readPos = 0;
	for (int tick = 0; tick < seconds * 200; tick++) {
		usleep(5000);
		uint32_t seq;
		uint64_t st, ht;
		do {
			seq = shm->audio.timestamp.sequence.load();
			st = shm->audio.timestamp.sampleTime.load();
			ht = shm->audio.timestamp.hostTime.load();
		} while ((seq & 1) || seq != shm->audio.timestamp.sequence.load());
		uint64_t nowTicks = mach_absolute_time();
		uint64_t elapsedTicks = nowTicks > ht ? nowTicks - ht : 0;
		uint64_t now = st + (uint64_t)((double)elapsedTicks / tps * rate);
		uint64_t ahead = now + safetyOut + rate / 100;
		if (written < now + safetyOut)
			written = now + safetyOut;
		float buf[4 * 512];
		while (written < ahead) {
			uint32_t n = ahead - written > 512 ? 512 : (uint32_t)(ahead - written);
			for (uint32_t i = 0; i < n; i++) {
				float s = (float)(0.25 * sin(phase));
				phase += 2 * M_PI * hz / rate;
				for (int c = 0; c < 4; c++)
					buf[4 * i + c] = channel < 0 || channel == c ? s : 0;
			}
			DynacordWriteOutput(shm->audio.outputBuffer, buf, written, n, ringOut, outBytes);
			written += n;
		}
		uint64_t inUpTo = now > safetyIn ? now - safetyIn : 0;
		if (readPos == 0 || inUpTo - readPos > ringIn)
			readPos = inUpTo;
		while (readPos < inUpTo) {
			uint32_t n = inUpTo - readPos > 512 ? 512 : (uint32_t)(inUpTo - readPos);
			DynacordReadInput(buf, shm->audio.inputBuffer, readPos, n, ringIn, inBytes);
			for (uint32_t i = 0; i < 4 * n; i++)
				peak[i % 4] = fmaxf(peak[i % 4], fabsf(buf[i]));
			readPos += n;
		}
		if (tick % 200 == 199) {
			printf("t=%3ds in dBFS:", (tick + 1) / 200);
			for (float& p : peak) {
				printf(" %6.1f", p > 0 ? 20 * log10f(p) : -144.0f);
				p = 0;
			}
			printf("\n");
			fflush(stdout);
		}
	}
	return 0;
}
