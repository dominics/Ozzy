/* SPDX-License-Identifier: MIT */
/*
 * Whether this user's daemon may drive one device: only while its user is the console user (in
 * front under fast user switching) and the device is attached. The daemon is a LaunchAgent in
 * each GUI session, so on a switch the old console user's daemon releases the device and its
 * segment and the new one's takes them. A start that finds the device busy (the other user's
 * daemon not yet let go, a kext or another process holding it) or that fails (a control request
 * timing out at plug-in) is retried: quickly at first, then slowly for as long as it is still
 * wanted. Pure state, no I/O: device.c and main.c do what the returned action says.
 */

#ifndef OZZY_DAEMON_HANDOVER_H
#define OZZY_DAEMON_HANDOVER_H

#include <stdbool.h>
#include <sys/types.h>

enum handover_action {
	HANDOVER_NONE,
	HANDOVER_START,        /* drive the device and report the result with handover_started */
	HANDOVER_STOP,         /* release the device and its segment */
	HANDOVER_RETRY_SOON,   /* call handover_retry after HANDOVER_SOON_MS */
	HANDOVER_RETRY_FAILED, /* call handover_retry after HANDOVER_FAILED_MS: the start failed */
	HANDOVER_RETRY_SLOWLY, /* call handover_retry after HANDOVER_SLOW_MS: slow to let go, or still failing */
	HANDOVER_GIVE_UP,      /* it can't be driven here: wait for a replug or a console change */
};

enum handover_start {
	HANDOVER_STARTED,
	HANDOVER_BUSY,         /* someone else holds the device or its segment */
	HANDOVER_FAILED,       /* the open, configuration or start failed: worth another try */
	HANDOVER_UNSUPPORTED,  /* not a device this daemon can drive (class compliant, no engine) */
};

#define HANDOVER_NO_CONSOLE   ((uid_t)-1)   /* the login window, or nobody */
#define HANDOVER_FAST_RETRIES 50
#define HANDOVER_SOON_MS      100
#define HANDOVER_SLOW_MS      2000
#define HANDOVER_FAILED_RETRIES 4   /* five starts in all, then slowly */
#define HANDOVER_FAILED_MS    500

struct handover {
	uid_t me;
	bool front;
	bool present;
	bool running;
	bool retry_pending;
	bool lost;               /* it was running and stopped by itself: retried slowly, failed or busy */
	unsigned int retries;    /* busy results this round */
	unsigned int failures;   /* failed starts this round */
};

/* Not in front and no device yet */
void handover_init(struct handover *h, uid_t me);

/* The console user is now @console_uid (HANDOVER_NO_CONSOLE at the login window) */
enum handover_action handover_console(struct handover *h, uid_t console_uid);

/* The device appeared or went away */
enum handover_action handover_device(struct handover *h, bool present);

/*
 * handover_started - What came of a HANDOVER_START. Busy is retried HANDOVER_FAST_RETRIES times
 * quickly, a failed start HANDOVER_FAILED_RETRIES times each HANDOVER_FAILED_MS, then either
 * slowly while the device is still wanted; a new round starts on a console change or a replug.
 * An unsupported device is given up on until then.
 */
enum handover_action handover_started(struct handover *h, enum handover_start result);

/* The retry delay has passed */
enum handover_action handover_retry(struct handover *h);

/*
 * handover_lost - The running device stopped and couldn't be restarted (a failed watchdog
 * recovery), and has been released. While it is still wanted it is retried each
 * HANDOVER_SLOW_MS, a failed start included, until one succeeds or a new round starts.
 */
enum handover_action handover_lost(struct handover *h);

#endif
