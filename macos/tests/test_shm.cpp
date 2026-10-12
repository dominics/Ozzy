/* SPDX-License-Identifier: MIT */
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <thread>
#include "../Backends/OzzyDaemon/SharedMemory.h"
#include "test.h"
#include "tests.h"

static const char* kTestName = "/ozzy_test_shm";

void test_shm_create_sets_header_and_mode()
{
	shm_unlink(kTestName);
	SharedMemorySegment seg(kTestName);
	CHECK(seg.Create() == SharedMemorySegment::CreateResult::kCreated);
	if (!seg.Get())
		return;
	CHECK_EQ(seg.Get()->magic, kOzzyMagic);
	CHECK_EQ(seg.Get()->version, kOzzyLayoutFrames);
	int fd = shm_open(kTestName, O_RDONLY, 0);
	struct stat sb;
	CHECK(fd >= 0 && fstat(fd, &sb) == 0);
	CHECK_EQ(sb.st_mode & 0777, 0666);   /* _coreaudiod must be able to map it */
	close(fd);
	seg.Destroy();
	CHECK(shm_open(kTestName, O_RDONLY, 0) < 0);
}

void test_shm_create_refuses_live_replaces_stale()
{
	shm_unlink(kTestName);
	SharedMemorySegment live(kTestName);
	CHECK(live.Create() == SharedMemorySegment::CreateResult::kCreated);
	if (!live.Get())
		return;

	/* A second instance while the first keeps ticking: refused, first untouched. */
	std::atomic<bool> ticking { true };
	std::thread ticker([&] {
		while (ticking) {
			live.TickHeartbeat();
			usleep(20000);
		}
	});
	SharedMemorySegment second(kTestName);
	CHECK(second.Create(150) == SharedMemorySegment::CreateResult::kInUse);
	CHECK_EQ(live.Get()->magic, kOzzyMagic);
	ticking = false;
	ticker.join();

	/* The first instance "crashes": stops ticking without unlinking. */
	SharedMemorySegment third(kTestName);
	CHECK(third.Create(150) == SharedMemorySegment::CreateResult::kReplacedStale);
	if (!third.Get())
		return;
	CHECK_EQ(third.Get()->magic, kOzzyMagic);
	CHECK_EQ(third.Get()->version, kOzzyLayoutFrames);
	third.Destroy();
	shm_unlink(kTestName);
}

void test_shm_destroy_keeps_replacement()
{
	shm_unlink(kTestName);
	SharedMemorySegment live(kTestName);
	CHECK(live.Create() == SharedMemorySegment::CreateResult::kCreated);
	if (!live.Get())
		return;

	/* live never ticks, so the replacement takes over the name. */
	SharedMemorySegment third(kTestName);
	CHECK(third.Create(150) == SharedMemorySegment::CreateResult::kReplacedStale);
	if (!third.Get())
		return;

	/* The stale instance's Destroy must leave the replacement's name alone. */
	live.Destroy();
	int fd = shm_open(kTestName, O_RDONLY, 0);
	CHECK(fd >= 0);
	if (fd >= 0) {
		void* p = mmap(nullptr, sizeof(OzzySharedMemory), PROT_READ, MAP_SHARED, fd, 0);
		close(fd);
		CHECK(p != MAP_FAILED);
		if (p != MAP_FAILED) {
			CHECK_EQ(((const OzzySharedMemory*)p)->magic, kOzzyMagic);
			munmap(p, sizeof(OzzySharedMemory));
		}
	}

	third.Destroy();
	shm_unlink(kTestName);
}

void test_shm_clear_stale_removes_only_dead_own_segment()
{
	shm_unlink(kTestName);
	SharedMemorySegment probe(kTestName);
	CHECK(!probe.ClearStale(150));                 /* nothing there */

	/* A live daemon keeps ticking: left alone. */
	SharedMemorySegment live(kTestName);
	CHECK(live.Create() == SharedMemorySegment::CreateResult::kCreated);
	if (!live.Get())
		return;
	std::atomic<bool> ticking { true };
	std::thread ticker([&] {
		while (ticking) {
			live.TickHeartbeat();
			usleep(20000);
		}
	});
	CHECK(!probe.ClearStale(150));
	CHECK_EQ(live.Get()->magic, kOzzyMagic);

	/* It "crashes": stops ticking without unlinking (as in the tests above). */
	ticking = false;
	ticker.join();
	CHECK(probe.ClearStale(150));
	CHECK(shm_open(kTestName, O_RDONLY, 0) < 0);
	live.Destroy();                                /* name already gone: no-op */
	shm_unlink(kTestName);
}
