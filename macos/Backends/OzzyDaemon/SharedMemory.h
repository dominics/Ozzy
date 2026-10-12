/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include "../../Shared/OzzySharedData.h"

/*
 * The POSIX shared memory segment the daemon publishes and OzzyHAL maps
 * (its "daemon" fallback). Mode 0666 regardless of umask, so coreaudiod's
 * _coreaudiod user can map it. Only one live daemon may own it.
 */
class SharedMemorySegment {
public:
	explicit SharedMemorySegment(const char* name = kOzzySharedMemName);
	~SharedMemorySegment();

	enum class CreateResult { kCreated, kReplacedStale, kInUse, kError };

	/* Create and initialise the segment. An existing segment owned by this
	 * user whose heartbeat has not moved for staleAfterMs is unlinked and
	 * replaced (a crashed daemon); anything live, foreign or unreadable is
	 * left alone and kInUse or kError returned. A half-created segment gets
	 * the same staleAfterMs window to finish before it counts as stale.
	 * Call before the process starts any other thread: it briefly clears
	 * the process umask. */
	CreateResult Create(uint64_t staleAfterMs = 1000);

	/* Unlink the named segment if it is owned by this user and its heartbeat
	 * has not moved for staleAfterMs (left by a crashed daemon). Leaves live
	 * or foreign segments alone. Returns true if it removed one. */
	bool ClearStale(uint64_t staleAfterMs = 1000);

	/* Short description of the last Create() result other than kCreated. */
	const char* Reason() const { return mReason; }

	OzzySharedMemory* Get() const { return mShm; }

	/* Device strings and IDs the HAL shows. */
	void PublishIdentity(uint16_t vid, uint16_t pid, const char* manufacturer, const char* product,
			     const char* serial);

	void TickHeartbeat();

	/* Pick a new random sessionID; the HAL resyncs its timestamps on a change. */
	void NewSession();

	/* Clear hardwarePresent, driverReady and magic and unmap. Unlinks the
	 * name only if it still refers to this segment, so a stale instance
	 * never removes a live replacement. Idempotent. */
	void Destroy();

private:
	bool MapNew();
	void SetReason(const char* what, int err);
	const char* mName;
	int mFd = -1;
	OzzySharedMemory* mShm = nullptr;
	char mReason[96] = "";
};
