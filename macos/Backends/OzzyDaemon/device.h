/* SPDX-License-Identifier: MIT */
/*
 * One device the daemon drives: its engine on a user_bus, published in its segment. What
 * OzzyKext is for the kext: the start, the settings and rate methods (here from the segment's
 * mailbox), the stall watchdog, sleep and wake, and the device going away. Everything here runs
 * on the daemon's control thread (the main run loop), under the device's lock.
 */

#ifndef OZZY_DAEMON_DEVICE_H
#define OZZY_DAEMON_DEVICE_H

#include "segment.h"
#include "user_bus.h"

struct daemon_device {
	struct daemon_device *next;
	io_service_t service;
	uint32_t location_id;
	const struct ozzy_device *model;

	pthread_mutex_t lock;     /* the engine's lifecycle: start, restart, stop (OzzyKext's mSettingsLock) */
	struct user_bus bus;
	struct segment seg;
	struct ozzy_engine *engine;
	void *engine_mem;         /* what engine was initialised in */
	bool running;
	bool sleeping;
	uint64_t recovered_at;

	CFRunLoopTimerRef poll;   /* the mailbox */
	CFRunLoopTimerRef watch;  /* the stall watchdog and the heartbeat */
};

/*
 * device_start - Drive the device at @service (a listed model, not class compliant, not held by
 * another driver): segment, configuration, engine start, timers on the current run loop. NULL
 * if the daemon leaves it alone or it failed (logged).
 */
struct daemon_device *device_start(io_service_t service, const struct ozzy_device *model);

/* The device is gone or the daemon quits: stop, close, remove the segment, free */
void device_stop(struct daemon_device *d);

/* System sleep: suspend (the HAL keeps the device). Wake: a new start. */
void device_sleep(struct daemon_device *d);
void device_wake(struct daemon_device *d);

#endif
