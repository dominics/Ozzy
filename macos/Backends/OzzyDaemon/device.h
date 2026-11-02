/* SPDX-License-Identifier: MIT */
/*
 * One device the daemon knows about: attached, a listed model it is allowed to drive. While
 * its user is in front and nobody else holds it, the daemon drives it: its engine on a
 * user_bus, published in its segment. What OzzyKext is for the kext: the start, the settings
 * and rate methods (here from the segment's mailbox), the stall watchdog, sleep and wake, and
 * the device going away. When to drive it is handover.h's decision. Everything here runs on
 * the daemon's control thread (the main run loop), under the device's lock.
 */

#ifndef OZZY_DAEMON_DEVICE_H
#define OZZY_DAEMON_DEVICE_H

#include "handover.h"
#include "segment.h"
#include "user_bus.h"

struct daemon_device {
	struct daemon_device *next;
	io_service_t service;
	uint32_t location_id;
	const struct ozzy_device *model;

	struct handover handover;
	CFRunLoopTimerRef retry;  /* a pending HANDOVER_RETRY_* */
	bool busy_logged;         /* why it is busy, logged once per round of retries */
	bool slow_logged;         /* that it went over to slow retries, likewise (busy or failing) */

	pthread_mutex_t lock;     /* the engine's lifecycle: start, restart, stop (OzzyKext's mSettingsLock) */
	bool driving;             /* bus, segment and engine set up (device_drive succeeded) */
	struct user_bus bus;
	struct segment seg;
	struct ozzy_engine *engine;
	void *engine_mem;         /* what engine was initialised in */
	bool running;
	bool sleeping;
	uint64_t recovered_at;
	/* A watchdog recovery failed: called on the control thread without the lock, to release it */
	void (*on_lost)(struct daemon_device *d);

	CFRunLoopTimerRef poll;   /* the mailbox */
	CFRunLoopTimerRef watch;  /* the stall watchdog and the heartbeat (seg ticks it during starts) */
};

/* Know the device at @service (retained), not yet driven. NULL if out of memory. */
struct daemon_device *device_new(io_service_t service, const struct ozzy_device *model);

/*
 * device_drive - Open the device and drive it: segment, configuration, engine start, timers on
 * the current run loop. HANDOVER_BUSY when another driver or process holds the device or its
 * segment, logged only if d->busy_logged is clear (and then set). HANDOVER_UNSUPPORTED for a
 * device it can't drive at all (class compliant, no engine). Anything else that fails is logged
 * and HANDOVER_FAILED. On anything but HANDOVER_STARTED the device is left closed.
 */
enum handover_start device_drive(struct daemon_device *d);

/*
 * device_clear_stale - Remove the device's segment if a crashed daemon of this user left it, so
 * the user in front can take the device. For a daemon that won't drive the device itself (one
 * that does replaces it in device_drive).
 */
void device_clear_stale(struct daemon_device *d);

/* Stop driving: stop the engine, close the device, remove the segment. Safe when not driving. */
void device_release(struct daemon_device *d);

/* The device is gone or the daemon quits: release and free */
void device_free(struct daemon_device *d);

/* System sleep: suspend (the HAL keeps the device). Wake: a new start. No-ops unless driving. */
void device_sleep(struct daemon_device *d);
void device_wake(struct daemon_device *d);

#endif
