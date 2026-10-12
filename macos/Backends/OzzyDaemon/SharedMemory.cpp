/* SPDX-License-Identifier: MIT */
#include "SharedMemory.h"
#include <errno.h>
#include <fcntl.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

SharedMemorySegment::SharedMemorySegment(const char* name) : mName(name) {}

SharedMemorySegment::~SharedMemorySegment() { Destroy(); }

void SharedMemorySegment::SetReason(const char* what, int err)
{
	if (err)
		snprintf(mReason, sizeof(mReason), "%s: %s", what, strerror(err));
	else
		snprintf(mReason, sizeof(mReason), "%s", what);
}

/* Decide whether the segment behind fd is held by a live writer. Live means
 * the heartbeat moves, or magic appears after it was missing, at any point
 * within staleAfterMs. A segment still unmapped-sized or unchanged for the
 * whole window is stale. A map failure is reported as live, so nothing is
 * unlinked that could not be inspected. */
static bool IsLive(int fd, uint64_t staleAfterMs)
{
	const OzzySharedMemory* shm = nullptr;
	bool firstMagicSet = false, live = false;
	uint32_t firstHeartbeat = 0;
	for (uint64_t waited = 0;; waited += 10) {
		struct stat sb;
		if (!shm && fstat(fd, &sb) == 0 && sb.st_size >= (off_t)sizeof(OzzySharedMemory)) {
			void* p = mmap(nullptr, sizeof(OzzySharedMemory), PROT_READ, MAP_SHARED, fd, 0);
			if (p == MAP_FAILED)
				return true;
			shm = (const OzzySharedMemory*)p;
			firstMagicSet = shm->magic == kOzzyMagic;
			firstHeartbeat = shm->heartbeat.load(std::memory_order_relaxed);
		}
		if (shm) {
			if (!firstMagicSet && shm->magic == kOzzyMagic)
				live = true;
			else if (shm->heartbeat.load(std::memory_order_relaxed) != firstHeartbeat)
				live = true;
		}
		if (live || waited >= staleAfterMs)
			break;
		usleep(10000);
	}
	if (shm)
		munmap((void*)shm, sizeof(OzzySharedMemory));
	return live;
}

bool SharedMemorySegment::MapNew()
{
	/* shm_open applies the umask to the create mode and fchmod() is refused
	 * on POSIX shm objects, so the umask is cleared for this call only. */
	mode_t oldMask = umask(0);
	int fd = shm_open(mName, O_RDWR | O_CREAT | O_EXCL, 0666);
	int openErrno = errno;
	umask(oldMask);
	if (fd < 0) {
		errno = openErrno;
		return false;
	}
	mFd = fd;
	if (ftruncate(mFd, sizeof(OzzySharedMemory)) != 0) {
		int e = errno;
		close(mFd);
		mFd = -1;
		shm_unlink(mName);
		errno = e;
		return false;
	}
	void* p = mmap(nullptr, sizeof(OzzySharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, mFd, 0);
	if (p == MAP_FAILED) {
		int e = errno;
		close(mFd);
		mFd = -1;
		shm_unlink(mName);
		errno = e;
		return false;
	}
	mShm = new (p) OzzySharedMemory();
	mShm->version = kOzzyLayoutFrames;
	mShm->magic = kOzzyMagic;
	return true;
}

SharedMemorySegment::CreateResult SharedMemorySegment::Create(uint64_t staleAfterMs)
{
	mReason[0] = '\0';
	if (MapNew())
		return CreateResult::kCreated;
	if (errno != EEXIST) {
		SetReason("shm_open", errno);
		return CreateResult::kError;
	}

	int fd = shm_open(mName, O_RDONLY, 0);
	if (fd < 0 && errno == ENOENT) {
		/* Removed since the failed create: take the fresh path. */
		if (MapNew())
			return CreateResult::kCreated;
		if (errno == EEXIST) {
			SetReason("in use by a live daemon", 0);
			return CreateResult::kInUse;
		}
		SetReason("shm_open", errno);
		return CreateResult::kError;
	}
	if (fd < 0) {
		int err = errno;
		SetReason("shm_open", err);
		return err == EACCES ? CreateResult::kInUse : CreateResult::kError;
	}

	struct stat sb;
	if (fstat(fd, &sb) != 0) {
		int e = errno;
		close(fd);
		SetReason("fstat", e);
		return CreateResult::kInUse;
	}
	if (sb.st_uid != getuid()) {
		close(fd);
		snprintf(mReason, sizeof(mReason), "owned by uid %u", (unsigned)sb.st_uid);
		return CreateResult::kInUse;
	}
	bool live = IsLive(fd, staleAfterMs);
	close(fd);
	if (live) {
		SetReason("in use by a live daemon", 0);
		return CreateResult::kInUse;
	}

	shm_unlink(mName);
	if (MapNew()) {
		SetReason("replaced stale segment", 0);
		return CreateResult::kReplacedStale;
	}
	if (errno == EEXIST) {
		SetReason("in use by a live daemon", 0);
		return CreateResult::kInUse;
	}
	SetReason("shm_open", errno);
	return CreateResult::kError;
}

bool SharedMemorySegment::ClearStale(uint64_t staleAfterMs)
{
	int fd = shm_open(mName, O_RDONLY, 0);
	if (fd < 0)
		return false;
	struct stat sb;
	bool stale = fstat(fd, &sb) == 0 && sb.st_uid == getuid() && !IsLive(fd, staleAfterMs);
	close(fd);
	return stale && shm_unlink(mName) == 0;
}

void SharedMemorySegment::PublishIdentity(uint16_t vid, uint16_t pid, const char* manufacturer, const char* product,
					  const char* serial)
{
	mShm->vendorID = vid;
	mShm->productID = pid;
	strlcpy(mShm->manufacturerName, manufacturer, sizeof(mShm->manufacturerName));
	strlcpy(mShm->productName, product, sizeof(mShm->productName));
	strlcpy(mShm->serialNumber, serial, sizeof(mShm->serialNumber));
}

void SharedMemorySegment::TickHeartbeat()
{
	mShm->heartbeat.fetch_add(1, std::memory_order_relaxed);
}

void SharedMemorySegment::NewSession()
{
	mShm->sessionID = arc4random();
}

void SharedMemorySegment::Destroy()
{
	if (mShm) {
		/* Stamp a marker so we can tell whether the name still maps to us. */
		uint32_t mark = arc4random() | 1;
		mShm->audio.driverReady = false;
		mShm->audio.hardwarePresent = false;
		mShm->magic = 0;
		mShm->heartbeat.store(mark);
		munmap(mShm, sizeof(OzzySharedMemory));
		mShm = nullptr;

		bool ours = false;
		int fd = shm_open(mName, O_RDONLY, 0);
		if (fd >= 0) {
			void* p = mmap(nullptr, sizeof(OzzySharedMemory), PROT_READ, MAP_SHARED, fd, 0);
			close(fd);
			if (p != MAP_FAILED) {
				const OzzySharedMemory* s = (const OzzySharedMemory*)p;
				ours = s->magic == 0 && s->heartbeat.load(std::memory_order_relaxed) == mark;
				munmap(p, sizeof(OzzySharedMemory));
			}
		}
		if (ours)
			shm_unlink(mName);
	}
	if (mFd >= 0) {
		close(mFd);
		mFd = -1;
	}
}
