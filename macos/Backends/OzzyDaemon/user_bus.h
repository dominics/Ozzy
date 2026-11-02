/* SPDX-License-Identifier: MIT */
/*
 * struct ozzy_bus over IOUSBLib for one device, in user space: what KextBus and
 * OzzyKext::ConfigureHardware are for the kext. Two real-time threads: the I/O thread runs a
 * CFRunLoop with the interfaces' async event sources (all completions arrive there), the
 * timer thread calls ozzy_engine_timer on absolute deadlines (as the WinUSB harness). The
 * engine's own calls (start, stop, restart) come from the daemon's control thread.
 */

#ifndef OZZY_DAEMON_USER_BUS_H
#define OZZY_DAEMON_USER_BUS_H

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <mach/mach_time.h>
#include <os/lock.h>
#include <pthread.h>
#include "../../../engine/core/ozzy_engine.h"
#include "../../Shared/OzzyDaemonShared.h"
#include "isoc_plan.h"

#define UBUS_PIPE_SLOTS      32
#define UBUS_MAX_INTERFACES  4
#define UBUS_REQUESTS        64

typedef IOUSBDeviceInterface650 **ubus_device_t;
typedef IOUSBInterfaceInterface650 **ubus_interface_t;

struct ubus_pipe {
	uint8_t address;          /* endpoint address with direction bit, 0 if the slot is unused */
	uint8_t ref;              /* IOUSBLib pipe ref on its interface */
	uint8_t type;             /* kUSBIsoc, kUSBBulk, kUSBInterrupt */
	uint8_t interval;         /* bInterval */
	uint8_t intf;             /* index into user_bus.intf */
	uint32_t uframes_per_packet;
	uint64_t next_frame;      /* 1 ms frame the next isochronous transfer starts at (0: pick one) */
	uint64_t last_done;       /* host time of the last completion (watchdog) */
	uint64_t last_data;       /* host time of the last isochronous IN packet with data */
	uint32_t error_streak;    /* failed completions in a row */
	uint32_t late;            /* isochronous submits since the last watch reset that found the
				     pipe's queue run dry (next_frame passed) */
	uint32_t refused;         /* submits refused since the last abort: transfers the engine lost */
	bool submitted;           /* something was submitted since the last watch reset */
};

struct ubus_request {
	struct user_bus *bus;
	struct ubus_pipe *pipe;
	void *xfer;
	struct ozzy_isoc_packet *packets;
	uint32_t count;
	uint8_t *buf;
	uint32_t buf_size;
	struct isoc_plan plan;
	uint64_t start_uframe;
	uint8_t *bounce;
	struct ubus_request *next_free;
	IOUSBIsocFrame frames[OZZY_MAX_ISOC_PACKETS];
};

struct ubus_interface {
	ubus_interface_t intf;
	uint8_t number;
	CFRunLoopSourceRef source;
};

struct user_bus {
	struct ozzy_bus bus;
	struct ozzy_engine *engine;
	struct ozzy_daemon_trailer *trailer;   /* MIDI out pending flag, checked at each timer tick */

	io_service_t service;
	ubus_device_t dev;
	bool dev_open;
	uint8_t speed;                         /* kUSBDeviceSpeed* */
	struct ubus_interface intf[UBUS_MAX_INTERFACES];
	unsigned int intf_count;
	struct ubus_pipe pipes[UBUS_PIPE_SLOTS];

	os_unfair_lock lock;                   /* the request pool, isochronous scheduling, the clock */
	struct ubus_request *requests;
	struct ubus_request *free_requests;
	uint8_t *bounce_mem;
	uint32_t bounce_size;
	int pending;                           /* requests IOKit holds */
	struct frame_clock clock;

	uint64_t host_hz;
	mach_timebase_info_data_t timebase;
	bool busy;                             /* an interface was held by another driver or process */

	pthread_t io_thread;
	CFRunLoopRef io_loop;
	bool io_running;

	pthread_t timer_thread;
	bool timer_on;
	bool timer_thread_live;
	uint32_t timer_interval_us;
};

/*
 * ubus_open - The device interface for @service, opened (USBDeviceOpen). Returns the IOReturn:
 * kIOReturnExclusiveAccess means another driver or process has the device (the kext).
 */
IOReturn ubus_open(struct user_bus *b, io_service_t service);

/* Interface 0 is USB Audio class: a class-compliant revision (the CMS 600-3's HW:03) */
bool ubus_class_compliant(struct user_bus *b);

/* The connection speed, as the engine counts it */
enum ozzy_usb_speed ubus_speed(const struct user_bus *b);

/* Start the I/O thread (the completions' run loop); before ubus_configure */
bool ubus_start_io(struct user_bus *b);

/*
 * ubus_configure - Configuration 1, the @engine profile's interfaces opened with their
 * alternate settings, the endpoints bound (ozzy_engine_bind_endpoints) and the pipes and
 * requests set up. As OzzyKext::ConfigureHardware. On failure b->busy says whether an
 * interface was held by another driver or process (worth a retry).
 */
bool ubus_configure(struct user_bus *b, struct ozzy_engine *engine);

/* Forget completions and submissions so far (after a start or restart) */
void ubus_watch_reset(struct user_bus *b);
/* Pipe @address: submitted since the last reset and no completion for @ticks */
bool ubus_pipe_stalled(struct user_bus *b, uint8_t address, uint64_t now, uint64_t ticks);
/* Pipe @address (isochronous IN): no packet with data for @ticks while submitted */
bool ubus_pipe_no_data(struct user_bus *b, uint8_t address, uint64_t now, uint64_t ticks);
/* Pipe @address: failed completions in a row */
uint32_t ubus_pipe_errors(struct user_bus *b, uint8_t address);
/* Pipe @address (isochronous): submits since the last reset that came after its queue ran dry */
uint32_t ubus_pipe_late(struct user_bus *b, uint8_t address);
/*
 * Pipe @address: submits refused since the last abort (the engine's start, restart or suspend
 * aborts first), each a transfer the engine won't submit again
 */
uint32_t ubus_pipe_refused(struct user_bus *b, uint8_t address);

/* Host clock ticks for @ms */
uint64_t ubus_ms_to_ticks(const struct user_bus *b, uint64_t ms);

/* Stop the timer and the I/O thread, abort what's in flight, close the interfaces and device */
void ubus_close(struct user_bus *b);

#endif
