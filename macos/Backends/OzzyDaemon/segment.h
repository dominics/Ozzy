/* SPDX-License-Identifier: MIT */
/*
 * The POSIX shared memory segment the daemon publishes for one device: struct
 * ozzy_shared_memory, then the fork's trailer (macos/Shared/OzzyDaemonShared.h). Mode 0666
 * whatever the umask, so coreaudiod's _coreaudiod user can map it. One live daemon owns a name:
 * a segment of the same name left by a crashed daemon (heartbeat not moving) is replaced, ours
 * always, another user's if the system lets us unlink it; anything live is left alone.
 */

#ifndef OZZY_DAEMON_SEGMENT_H
#define OZZY_DAEMON_SEGMENT_H

#include <dispatch/dispatch.h>
#include "../../../engine/shared/ozzy_shared_data.h"
#include "../../Shared/OzzyDaemonShared.h"

enum segment_result {
	SEGMENT_CREATED,
	SEGMENT_REPLACED_STALE,
	SEGMENT_IN_USE,
	SEGMENT_ERROR,
};

struct segment {
	char name[OZZY_DAEMON_SEGMENT_NAME_MAX];
	int fd;
	size_t size;
	void *base;
	volatile struct ozzy_shared_memory *shm;
	struct ozzy_daemon_trailer *trailer;
	char reason[96];

	/* segment_beat_start: the hold heartbeat */
	dispatch_queue_t beat_queue;
	dispatch_source_t beat;
	uint64_t held_since_ns;   /* __atomic; 0 = not held */
};

/* How long a hold keeps the heartbeat moving at most */
enum { SEGMENT_HOLD_MAX_MS = 5000 };

/*
 * segment_create - Create, map and zero the segment @name (shm, then the trailer with its
 * magic). @stale_after_ms: how long an existing segment's heartbeat must stand still to count
 * as left by a crashed daemon. A stale segment of another user's that can't be unlinked is
 * SEGMENT_IN_USE, the shm_unlink error in seg->reason. Briefly clears the process umask: call
 * it from one thread at a time. seg->reason says why on anything but SEGMENT_CREATED.
 */
enum segment_result segment_create(struct segment *seg, const char *name, unsigned int stale_after_ms);

/*
 * segment_clear_stale - Unlink the segment @name if this user owns it and its heartbeat stands
 * still for @stale_after_ms: one left by this user's crashed daemon, which would otherwise keep
 * the device from the user in front. True if it removed one. Blocks up to @stale_after_ms when
 * there is one of ours to inspect.
 */
bool segment_clear_stale(const char *name, unsigned int stale_after_ms);

/* Advance the heartbeat (what tells a live segment from a stale one) */
void segment_tick(struct segment *seg);

/*
 * segment_beat_start - Keep the heartbeat moving from a queue of its own while the control
 * thread is held up in a call it can't tick from: an engine start or restart, whose handshake
 * takes about a second, which the plug-ins would otherwise read as a dead daemon.
 * segment_hold / segment_release bracket such a call. A hold ticks for SEGMENT_HOLD_MAX_MS at
 * most, so a control thread stuck in one still reads as hung. Call once the segment is mapped;
 * segment_destroy stops it.
 */
void segment_beat_start(struct segment *seg, unsigned int period_ms);
void segment_hold(struct segment *seg);
void segment_release(struct segment *seg);

/* segment_beat_stop - Stop the hold heartbeat and wait for a tick in progress. Safe when not started. */
void segment_beat_stop(struct segment *seg);

/*
 * segment_destroy - Clear hardware_present, driver_ready and the magics and unmap. Unlinks the
 * name only if it still refers to this segment, so a stale instance never removes a live
 * replacement. Safe to call twice.
 */
void segment_destroy(struct segment *seg);

/*
 * segment_abandon - segment_destroy, but the mapping is left in place (and so lost): for a
 * segment that transfers IOKit hasn't given back may still write into. The plug-ins see the
 * device go, and the name is unlinked, as segment_destroy does.
 */
void segment_abandon(struct segment *seg);

#endif
