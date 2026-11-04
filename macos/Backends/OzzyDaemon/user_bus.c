/* SPDX-License-Identifier: MIT */
/* struct ozzy_bus over IOUSBLib (user_bus.h) */

#include "user_bus.h"
#include <IOKit/IOCFPlugIn.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <stdlib.h>
#include <unistd.h>
#include "../../../engine/shared/ozzy_log.h"

#define ISOC_LEAD_FRAMES 10   /* the first transfer of a pipe starts this many 1 ms frames out (as the kext) */
#define ABORT_WAIT_MS    2000

static int pipe_slot(uint8_t address)
{
	return (address & 0x0F) | ((address & 0x80) >> 3);
}

static struct ubus_pipe *find_pipe(struct user_bus *b, uint8_t address)
{
	struct ubus_pipe *p = &b->pipes[pipe_slot(address)];

	return p->address == address ? p : NULL;
}

/* A short IN transfer or packet (the feedback's 3 bytes in a larger slot, a short bulk read)
 * completes as kIOReturnUnderrun: data for the engine, not an error */
static int ubus_status(uint8_t address, IOReturn kr)
{
	if ((address & 0x80) && kr == kIOReturnUnderrun)
		return 0;
	return (int)kr;
}

uint64_t ubus_ms_to_ticks(const struct user_bus *b, uint64_t ms)
{
	return ms * 1000000ull * b->timebase.denom / b->timebase.numer;
}

static uint64_t us_to_ticks(const struct user_bus *b, uint64_t us)
{
	return us * 1000ull * b->timebase.denom / b->timebase.numer;
}

/* Run the calling thread as a real-time thread (period, computation, constraint in us) */
static void make_realtime(const struct user_bus *b, uint32_t period, uint32_t computation, uint32_t constraint)
{
	thread_time_constraint_policy_data_t p = {
		.period = (uint32_t)us_to_ticks(b, period),
		.computation = (uint32_t)us_to_ticks(b, computation),
		.constraint = (uint32_t)us_to_ticks(b, constraint),
		.preemptible = TRUE,
	};
	kern_return_t kr = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
					     (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);

	if (kr != KERN_SUCCESS)
		ozzy_log("real-time policy refused: %s", mach_error_string(kr));
}

static void *query_plugin(io_service_t service, CFUUIDRef type, CFUUIDRef iid)
{
	IOCFPlugInInterface **plug = NULL;
	SInt32 score = 0;
	void *out = NULL;

	if (IOCreatePlugInInterfaceForService(service, type, kIOCFPlugInInterfaceID, &plug, &score) != kIOReturnSuccess ||
	    !plug)
		return NULL;
	(*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(iid), (LPVOID *)&out);
	IODestroyPlugInInterface(plug);
	return out;
}

/* --- Requests --- */

static struct ubus_request *acquire(struct user_bus *b)
{
	struct ubus_request *r;

	os_unfair_lock_lock(&b->lock);
	r = b->free_requests;
	if (r)
		b->free_requests = r->next_free;
	os_unfair_lock_unlock(&b->lock);
	return r;
}

static void release(struct user_bus *b, struct ubus_request *r)
{
	os_unfair_lock_lock(&b->lock);
	r->next_free = b->free_requests;
	b->free_requests = r;
	os_unfair_lock_unlock(&b->lock);
}

static void note_completion(struct ubus_pipe *p, int status, bool has_data)
{
	uint64_t now = mach_absolute_time();

	__atomic_store_n(&p->last_done, now, __ATOMIC_RELAXED);
	if (has_data)
		__atomic_store_n(&p->last_data, now, __ATOMIC_RELAXED);
	if (status == 0)
		__atomic_store_n(&p->error_streak, 0u, __ATOMIC_RELAXED);
	else if (status != OZZY_STATUS_ABORTED)
		__atomic_fetch_add(&p->error_streak, 1u, __ATOMIC_RELAXED);
}

/* A submit that won't happen: counted for the watchdog (ubus_pipe_refused) */
static bool refuse(struct ubus_pipe *p)
{
	__atomic_fetch_add(&p->refused, 1u, __ATOMIC_RELAXED);
	return false;
}

/* --- Bulk and interrupt --- */

static void bulk_done(void *refcon, IOReturn result, void *arg0)
{
	struct ubus_request *r = refcon;
	struct user_bus *b = r->bus;
	struct ubus_pipe *p = r->pipe;
	uint8_t address = p->address;
	void *xfer = r->xfer;
	int status = ubus_status(address, result);

	release(b, r);
	note_completion(p, status, false);
	if (status != 0 && status != OZZY_STATUS_ABORTED)
		ozzy_log("pipe 0x%02x error: 0x%x", address, (unsigned int)result);
	if (b->engine)
		ozzy_engine_complete(b->engine, address, xfer, status, (uint32_t)(uintptr_t)arg0);
	__atomic_fetch_sub(&b->pending, 1, __ATOMIC_RELEASE);
}

static bool ubus_submit(void *ctx, uint8_t pipe, void *buf, uint32_t size, void *xfer)
{
	struct user_bus *b = ctx;
	struct ubus_pipe *p = find_pipe(b, pipe);
	struct ubus_request *r;
	ubus_interface_t intf;
	IOReturn kr;

	if (!p)
		return false;
	r = acquire(b);
	if (!r) {
		ozzy_log("pipe 0x%02x: no free request", pipe);
		return refuse(p);
	}
	r->pipe = p;
	r->xfer = xfer;
	r->packets = NULL;
	r->count = 0;
	intf = b->intf[p->intf].intf;
	__atomic_fetch_add(&b->pending, 1, __ATOMIC_ACQ_REL);
	kr = (pipe & 0x80) ? (*intf)->ReadPipeAsync(intf, p->ref, buf, size, bulk_done, r)
			   : (*intf)->WritePipeAsync(intf, p->ref, buf, size, bulk_done, r);
	if (kr != kIOReturnSuccess) {
		__atomic_fetch_sub(&b->pending, 1, __ATOMIC_RELEASE);
		release(b, r);
		ozzy_log("pipe 0x%02x submit failed: 0x%x", pipe, (unsigned int)kr);
		return refuse(p);
	}
	__atomic_store_n(&p->submitted, true, __ATOMIC_RELAXED);
	return true;
}

/* --- Isochronous --- */

/* The bus's microframe number and a host time at or after it began (within 1 ms) */
static bool bus_uframe(struct user_bus *b, uint64_t *uframe, uint64_t *host)
{
	UInt64 n = 0;
	AbsoluteTime at;
	IOReturn kr;

	if (b->speed >= kUSBDeviceSpeedHigh) {
		kr = (*b->dev)->GetBusMicroFrameNumber(b->dev, &n, &at);
	} else {
		kr = (*b->dev)->GetBusFrameNumber(b->dev, &n, &at);
		n *= 8;
	}
	if (kr != kIOReturnSuccess)
		return false;
	*uframe = n;
	memcpy(host, &at, sizeof(*host));
	return true;
}

static void isoc_done(void *refcon, IOReturn result, void *arg0)
{
	struct ubus_request *r = refcon;
	struct user_bus *b = r->bus;
	struct ubus_pipe *p = r->pipe;
	uint8_t address = p->address;
	bool in = address & 0x80, has_data = false;
	struct ozzy_isoc_packet *packets = r->packets;
	uint32_t count = r->count, i;
	void *xfer = r->xfer;
	int status = ubus_status(address, result);

	for (i = 0; i < count; i++) {
		packets[i].actual = r->frames[i].frActCount;
		packets[i].status = ubus_status(address, r->frames[i].frStatus);
		if (in && packets[i].status == 0 && packets[i].actual)
			has_data = true;
	}
	if (in)
		isoc_plan_scatter(&r->plan, r->buf, r->buf_size, packets, count);
	release(b, r);
	note_completion(p, status, has_data);
	if (status != 0 && status != OZZY_STATUS_ABORTED)
		ozzy_log("isoc pipe 0x%02x error: 0x%x", address, (unsigned int)result);
	if (b->engine)
		ozzy_engine_isoc_complete(b->engine, address, xfer, status, packets, count);
	__atomic_fetch_sub(&b->pending, 1, __ATOMIC_RELEASE);
}

/*
 * As KextBus::SubmitIsoc, with the frame io(): the frame list goes back to back from one
 * buffer (isoc_plan). Each transfer starts where the pipe's previous one ended, the first
 * ISOC_LEAD_FRAMES ahead of the bus. A transfer whose start has already passed means the
 * pipe's whole queue ran dry: it starts ISOC_LEAD_FRAMES out instead and is counted as late
 * (ubus_pipe_late), since the device has had a hole in the stream.
 */
static bool ubus_submit_isoc(void *ctx, uint8_t pipe, void *buf, uint32_t buf_size, struct ozzy_isoc_packet *packets,
			     uint32_t count, void *xfer)
{
	struct user_bus *b = ctx;
	struct ubus_pipe *p = find_pipe(b, pipe);
	struct ubus_request *r;
	ubus_interface_t intf;
	uint64_t uframe = 0, host = 0, first, frames, behind;
	bool in = pipe & 0x80, have_clock, late;
	IOReturn kr;
	uint32_t i;

	if (!p)
		return false;
	if (p->type != kUSBIsoc || count == 0 || count > OZZY_MAX_ISOC_PACKETS)
		return refuse(p);
	r = acquire(b);
	if (!r) {
		ozzy_log("isoc pipe 0x%02x: no free request", pipe);
		return refuse(p);
	}
	if (!isoc_plan_make(&r->plan, buf, buf_size, packets, count, r->bounce, b->bounce_size, !in)) {
		release(b, r);
		ozzy_log("isoc pipe 0x%02x: %u packets don't fit the bounce buffer", pipe, count);
		return refuse(p);
	}
	for (i = 0; i < count; i++) {
		r->frames[i].frStatus = kIOReturnSuccess;
		r->frames[i].frReqCount = (UInt16)packets[i].length;
		r->frames[i].frActCount = 0;
	}
	r->pipe = p;
	r->xfer = xfer;
	r->packets = packets;
	r->count = count;
	r->buf = buf;
	r->buf_size = buf_size;

	frames = ((uint64_t)count * p->uframes_per_packet + 7) / 8;
	have_clock = bus_uframe(b, &uframe, &host);   /* a call into the kernel: outside the lock */
	os_unfair_lock_lock(&b->lock);
	if (have_clock)
		frame_clock_sample(&b->clock, uframe, host);
	late = isoc_schedule(&p->next_frame, uframe / 8, frames, ISOC_LEAD_FRAMES, &first, &behind);
	r->start_uframe = first * 8;
	isoc_plan_times(packets, count, &b->clock, r->start_uframe, p->uframes_per_packet);
	os_unfair_lock_unlock(&b->lock);

	intf = b->intf[p->intf].intf;
	__atomic_fetch_add(&b->pending, 1, __ATOMIC_ACQ_REL);
	kr = in ? (*intf)->ReadIsochPipeAsync(intf, p->ref, r->plan.data, first, count, r->frames, isoc_done, r)
		: (*intf)->WriteIsochPipeAsync(intf, p->ref, r->plan.data, first, count, r->frames, isoc_done, r);
	if (kr != kIOReturnSuccess) {
		__atomic_fetch_sub(&b->pending, 1, __ATOMIC_RELEASE);
		os_unfair_lock_lock(&b->lock);
		p->next_frame = first;
		os_unfair_lock_unlock(&b->lock);
		release(b, r);
		ozzy_log("isoc pipe 0x%02x submit failed: 0x%x (frame %llu, bus %llu)", pipe, (unsigned int)kr,
			 (unsigned long long)first, (unsigned long long)(uframe / 8));
		return refuse(p);
	}
	__atomic_store_n(&p->submitted, true, __ATOMIC_RELAXED);
	if (late) {
		__atomic_fetch_add(&p->late, 1u, __ATOMIC_RELAXED);
		ozzy_log("isoc pipe 0x%02x: late, its queue ran dry %llu frame(s) ago; resumed at frame %llu", pipe,
			 (unsigned long long)behind, (unsigned long long)first);
	}
	return true;
}

/* --- Control, halt, reselect --- */

static bool ubus_control(void *ctx, uint8_t type, uint8_t request, uint16_t value, uint16_t index, void *data,
			 uint16_t length)
{
	struct user_bus *b = ctx;
	IOUSBDevRequestTO req = {
		.bmRequestType = type,
		.bRequest = request,
		.wValue = value,
		.wIndex = index,
		.wLength = length,
		.pData = data,
		.noDataTimeout = 1000,
		.completionTimeout = 5000,
	};
	IOReturn kr = (*b->dev)->DeviceRequestTO(b->dev, &req);

	if (kr != kIOReturnSuccess)
		ozzy_log("control request 0x%02x (type 0x%02x, wValue 0x%04x, wIndex 0x%04x) failed: 0x%x", request, type,
			 value, index, (unsigned int)kr);
	return kr == kIOReturnSuccess;
}

static int ubus_clear_halt(void *ctx, uint8_t endpoint)
{
	struct user_bus *b = ctx;
	struct ubus_pipe *p = find_pipe(b, endpoint);
	ubus_interface_t intf;
	IOReturn kr;

	if (!p)
		return -ENODEV;
	intf = b->intf[p->intf].intf;
	kr = (*intf)->ClearPipeStallBothEnds(intf, p->ref);
	if (kr == kIOReturnSuccess)
		return 0;
	ozzy_log("clear halt on pipe 0x%02x failed: 0x%x", endpoint, (unsigned int)kr);
	return -EIO;
}

/* SET_INTERFACE @alt; a stall asked again (up to 10 times, 20 ms apart), as the kext */
static bool select_alternate(struct ubus_interface *i, uint8_t alt)
{
	IOReturn kr = kIOReturnError;
	int attempt;

	for (attempt = 0; attempt < 10; attempt++) {
		if (attempt)
			usleep(20000);
		kr = (*i->intf)->SetAlternateInterface(i->intf, alt);
		if (kr != kIOUSBPipeStalled && kr != kIOReturnNotResponding)
			break;
	}
	if (kr != kIOReturnSuccess)
		ozzy_log("interface %u: alternate setting %u failed: 0x%x", i->number, alt, (unsigned int)kr);
	return kr == kIOReturnSuccess;
}

/*
 * The pipes of the claimed interfaces on their current alternate settings: each endpoint's
 * descriptor (from the configuration descriptor, with the high-bandwidth bits the engine
 * reads) and its IOUSBLib pipe ref. @eps (may be NULL) gets the endpoints for the engine.
 */
static unsigned int load_pipes(struct user_bus *b, struct ozzy_usb_endpoint *eps, unsigned int max)
{
	IOUSBConfigurationDescriptorPtr cd = NULL;
	unsigned int n = 0, i;

	uint8_t desc_interval[UBUS_PIPE_SLOTS] = { 0 };

	memset(b->pipes, 0, sizeof(b->pipes));
	if ((*b->dev)->GetConfigurationDescriptorPtr(b->dev, 0, &cd) != kIOReturnSuccess || !cd)
		return 0;
	for (i = 0; i < b->intf_count; i++) {
		ubus_interface_t intf = b->intf[i].intf;
		const uint8_t *d = (const uint8_t *)cd, *end = d + USBToHostWord(cd->wTotalLength);
		UInt8 alt = 0, count = 0, ref;
		bool current = false;

		(*intf)->GetAlternateSetting(intf, &alt);
		for (; d + 2 <= end && d[0] >= 2 && d + d[0] <= end; d += d[0]) {
			if (d[1] == kUSBInterfaceDesc && d[0] >= 4) {
				current = d[2] == b->intf[i].number && d[3] == alt;
			} else if (d[1] == kUSBEndpointDesc && d[0] >= 7 && current) {
				desc_interval[pipe_slot(d[2])] = d[6];
				if (!eps || n >= max)
					continue;
				eps[n].intf = b->intf[i].number;
				eps[n].address = d[2];
				eps[n].attributes = d[3];
				eps[n].max_packet = (uint16_t)(d[4] | (d[5] << 8));
				eps[n].interval = d[6];
				eps[n++].handle = &b->intf[i];
			}
		}
		(*intf)->GetNumEndpoints(intf, &count);
		for (ref = 1; ref <= count; ref++) {
			UInt8 dir = 0, number = 0, type = 0, interval = 0;
			UInt16 mps = 0;
			struct ubus_pipe *p;
			uint8_t address, shift;

			if ((*intf)->GetPipeProperties(intf, ref, &dir, &number, &type, &mps, &interval) != kIOReturnSuccess)
				continue;
			address = (uint8_t)(number | (dir == kUSBIn ? 0x80 : 0));
			p = &b->pipes[pipe_slot(address)];
			p->address = address;
			p->ref = ref;
			p->type = type;
			/* The descriptor's bInterval, as the engine sees it */
			p->interval = desc_interval[pipe_slot(address)] ? desc_interval[pipe_slot(address)] : interval;
			p->intf = (uint8_t)i;
			/* Isochronous bInterval n: 2^(n-1) service intervals (microframes at high speed,
			 * frames at full speed) */
			shift = (uint8_t)((p->interval ? p->interval : 1) - 1);
			if (shift > 7)
				shift = 7;
			p->uframes_per_packet = (b->speed >= kUSBDeviceSpeedHigh ? 1u : 8u) << shift;
		}
	}
	return n;
}

/* As OzzyKext::Reselect: alternate setting 0, then the profile's, on each interface whose
 * setting isn't 0; the pipes are looked up again (their refs belong to the setting) */
static bool ubus_reselect(void *ctx, const uint8_t alts[4])
{
	struct user_bus *b = ctx;
	bool ok = true;
	unsigned int i;

	for (i = 0; i < b->intf_count && i < 4; i++) {
		if (!alts[i])
			continue;
		if (!select_alternate(&b->intf[i], 0))
			ok = false;
		if (!select_alternate(&b->intf[i], alts[i]))
			ok = false;
		usleep(50000);
	}
	os_unfair_lock_lock(&b->lock);
	load_pipes(b, NULL, 0);
	os_unfair_lock_unlock(&b->lock);
	return ok;
}

/* --- Timer --- */

static void ubus_stop_timer(void *ctx);

static void *timer_main(void *arg)
{
	struct user_bus *b = arg;
	uint64_t interval = us_to_ticks(b, b->timer_interval_us);
	uint64_t deadline = mach_absolute_time() + interval, now;

	pthread_setname_np("ozzy-timer");
	make_realtime(b, b->timer_interval_us, b->timer_interval_us / 4 + 50, b->timer_interval_us / 2 + 100);
	while (__atomic_load_n(&b->timer_on, __ATOMIC_ACQUIRE)) {
		mach_wait_until(deadline);
		if (!__atomic_load_n(&b->timer_on, __ATOMIC_ACQUIRE))
			break;
		if (b->trailer && ozzy_daemon_take_midi_out_pending(b->trailer))
			ozzy_engine_midi_out_pending(b->engine);
		ozzy_engine_timer(b->engine);
		/* Next deadline from the last; if we fell behind, the next one ahead */
		deadline += interval;
		now = mach_absolute_time();
		if (deadline <= now)
			deadline = now + interval;
	}
	return NULL;
}

static bool ubus_start_timer(void *ctx, uint32_t interval_us)
{
	struct user_bus *b = ctx;

	if (!interval_us)
		interval_us = 1000;
	if (b->timer_thread_live &&
	    (b->timer_interval_us == interval_us || pthread_equal(pthread_self(), b->timer_thread)))
		return true;
	ubus_stop_timer(b);   /* another interval: a new thread with it */
	b->timer_interval_us = interval_us;
	__atomic_store_n(&b->timer_on, true, __ATOMIC_RELEASE);
	if (pthread_create(&b->timer_thread, NULL, timer_main, b) != 0) {
		__atomic_store_n(&b->timer_on, false, __ATOMIC_RELEASE);
		return false;
	}
	b->timer_thread_live = true;
	return true;
}

/* Waits for a call in progress (the thread ends after it) */
static void ubus_stop_timer(void *ctx)
{
	struct user_bus *b = ctx;

	__atomic_store_n(&b->timer_on, false, __ATOMIC_RELEASE);
	if (!b->timer_thread_live)
		return;
	if (pthread_equal(pthread_self(), b->timer_thread)) {
		pthread_detach(b->timer_thread);   /* from the timer itself: it ends when this call returns */
	} else {
		pthread_join(b->timer_thread, NULL);
	}
	b->timer_thread_live = false;
}

/* --- The rest --- */

static void ubus_sleep(void *ctx, unsigned int ms)
{
	(void)ctx;
	usleep(ms * 1000);
}

static uint64_t ubus_time(void *ctx)
{
	(void)ctx;
	return mach_absolute_time();
}

static uint64_t ubus_time_hz(void *ctx)
{
	return ((struct user_bus *)ctx)->host_hz;
}

/* Abort what is in flight and wait until the I/O thread has delivered it */
static void ubus_abort(void *ctx)
{
	struct user_bus *b = ctx;
	int i, waited;

	for (i = 0; i < UBUS_PIPE_SLOTS; i++) {
		struct ubus_pipe *p = &b->pipes[i];

		if (p->address)
			(*b->intf[p->intf].intf)->AbortPipe(b->intf[p->intf].intf, p->ref);
	}
	if (b->io_loop && CFRunLoopGetCurrent() == b->io_loop) {
		ozzy_log("abort from a completion: not waiting");
	} else {
		for (waited = 0; waited < ABORT_WAIT_MS && __atomic_load_n(&b->pending, __ATOMIC_ACQUIRE) > 0; waited++)
			usleep(1000);
		if (__atomic_load_n(&b->pending, __ATOMIC_ACQUIRE) > 0)
			ozzy_log("%d transfers still pending after abort", __atomic_load_n(&b->pending, __ATOMIC_ACQUIRE));
	}
	os_unfair_lock_lock(&b->lock);
	for (i = 0; i < UBUS_PIPE_SLOTS; i++) {
		b->pipes[i].next_frame = 0;
		__atomic_store_n(&b->pipes[i].refused, 0u, __ATOMIC_RELAXED);
	}
	os_unfair_lock_unlock(&b->lock);
}

/* No PCM rings (the HAL writes the shared memory itself) and no stats_changed (the HAL reads
 * the stats from the shared memory), as KextBus */
static const struct ozzy_bus_ops ubus_ops = {
	.submit = ubus_submit,
	.submit_isoc = ubus_submit_isoc,
	.control = ubus_control,
	.clear_halt = ubus_clear_halt,
	.start_timer = ubus_start_timer,
	.stop_timer = ubus_stop_timer,
	.sleep_ms = ubus_sleep,
	.time = ubus_time,
	.abort = ubus_abort,
	.time_hz = ubus_time_hz,
	.reselect = ubus_reselect,
};

/* --- Setup --- */

IOReturn ubus_open(struct user_bus *b, io_service_t service)
{
	IOReturn kr;

	b->lock = OS_UNFAIR_LOCK_INIT;
	mach_timebase_info(&b->timebase);
	b->host_hz = 1000000000ull * b->timebase.denom / b->timebase.numer;
	frame_clock_init(&b->clock, b->host_hz);
	b->bus.ops = &ubus_ops;
	b->bus.ctx = b;

	IOObjectRetain(service);
	b->service = service;
	b->dev = query_plugin(service, kIOUSBDeviceUserClientTypeID, kIOUSBDeviceInterfaceID650);
	if (!b->dev)
		return kIOReturnUnsupported;
	(*b->dev)->GetDeviceSpeed(b->dev, &b->speed);
	kr = (*b->dev)->USBDeviceOpen(b->dev);
	b->dev_open = kr == kIOReturnSuccess;
	return kr;
}

bool ubus_class_compliant(struct user_bus *b)
{
	IOUSBConfigurationDescriptorPtr cd = NULL;
	const uint8_t *d, *end;

	if ((*b->dev)->GetConfigurationDescriptorPtr(b->dev, 0, &cd) != kIOReturnSuccess || !cd)
		return false;
	d = (const uint8_t *)cd;
	end = d + USBToHostWord(cd->wTotalLength);
	for (; d + 2 <= end && d[0] >= 2 && d + d[0] <= end; d += d[0]) {
		if (d[1] == kUSBInterfaceDesc && d[0] >= 6)
			return d[5] == kUSBAudioInterfaceClass;
	}
	return false;
}

enum ozzy_usb_speed ubus_speed(const struct user_bus *b)
{
	switch (b->speed) {
	case kUSBDeviceSpeedLow: return OZZY_USB_SPEED_LOW;
	case kUSBDeviceSpeedFull: return OZZY_USB_SPEED_FULL;
	case kUSBDeviceSpeedHigh: return OZZY_USB_SPEED_HIGH;
	default: return b->speed > kUSBDeviceSpeedHigh ? OZZY_USB_SPEED_SUPER : OZZY_USB_SPEED_UNKNOWN;
	}
}

static void keep_alive(CFRunLoopTimerRef timer, void *info)
{
	(void)timer;
	(void)info;
}

struct io_start {
	struct user_bus *b;
	dispatch_semaphore_t ready;
};

/* The completions' run loop: interface event sources are added to it from ubus_configure */
static void *io_main(void *arg)
{
	struct io_start *start = arg;
	struct user_bus *b = start->b;
	CFRunLoopTimerRef keep;

	pthread_setname_np("ozzy-io");
	make_realtime(b, 1000, 250, 500);
	keep = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 1e9, 1e9, 0, 0, keep_alive, NULL);
	CFRunLoopAddTimer(CFRunLoopGetCurrent(), keep, kCFRunLoopDefaultMode);
	b->io_loop = (CFRunLoopRef)CFRetain(CFRunLoopGetCurrent());
	dispatch_semaphore_signal(start->ready);
	CFRunLoopRun();
	CFRunLoopTimerInvalidate(keep);
	CFRelease(keep);
	return NULL;
}

bool ubus_start_io(struct user_bus *b)
{
	struct io_start start = { b, dispatch_semaphore_create(0) };

	if (pthread_create(&b->io_thread, NULL, io_main, &start) != 0) {
		dispatch_release(start.ready);
		return false;
	}
	dispatch_semaphore_wait(start.ready, DISPATCH_TIME_FOREVER);
	dispatch_release(start.ready);
	b->io_running = true;
	return true;
}

static void stop_io(struct user_bus *b)
{
	if (!b->io_running)
		return;
	CFRunLoopStop(b->io_loop);
	pthread_join(b->io_thread, NULL);
	CFRelease(b->io_loop);
	b->io_loop = NULL;
	b->io_running = false;
}

static bool open_interfaces(struct user_bus *b, const struct ozzy_device_profile *profile)
{
	IOUSBFindInterfaceRequest req = { kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare,
					  kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare };
	io_iterator_t it = IO_OBJECT_NULL;
	io_service_t s;
	unsigned int found = 0;
	bool ok = true;

	if ((*b->dev)->CreateInterfaceIterator(b->dev, &req, &it) != kIOReturnSuccess)
		return false;
	while (ok && (s = IOIteratorNext(it)) != IO_OBJECT_NULL) {
		ubus_interface_t intf = query_plugin(s, kIOUSBInterfaceUserClientTypeID, kIOUSBInterfaceInterfaceID650);
		UInt8 number = 0;
		int want = -1, i;
		IOReturn kr;

		IOObjectRelease(s);
		if (!intf)
			continue;
		(*intf)->GetInterfaceNumber(intf, &number);
		for (i = 0; i < profile->interface_count && i < UBUS_MAX_INTERFACES; i++) {
			if (profile->interfaces[i] == number && !b->intf[i].intf)
				want = i;
		}
		if (want < 0) {
			(*intf)->Release(intf);
			continue;
		}
		kr = (*intf)->USBInterfaceOpen(intf);
		if (kr != kIOReturnSuccess) {
			/* Held by another driver or process: the caller logs it, once per round of retries */
			if (kr == kIOReturnExclusiveAccess)
				b->busy = true;
			else
				ozzy_log("interface %u: open failed: 0x%x", number, (unsigned int)kr);
			(*intf)->Release(intf);
			ok = false;
			break;
		}
		b->intf[want].intf = intf;
		b->intf[want].number = number;
		found++;
		if (profile->alt_settings[want]) {
			if (!select_alternate(&b->intf[want], profile->alt_settings[want]))
				ok = false;
			usleep(50000);
		}
		kr = (*intf)->CreateInterfaceAsyncEventSource(intf, &b->intf[want].source);
		if (kr != kIOReturnSuccess) {
			ozzy_log("interface %u: no event source: 0x%x", number, (unsigned int)kr);
			ok = false;
		} else {
			CFRunLoopAddSource(b->io_loop, b->intf[want].source, kCFRunLoopDefaultMode);
		}
	}
	IOObjectRelease(it);
	b->intf_count = profile->interface_count;
	if (ok && found != profile->interface_count) {
		ozzy_log("found %u of the profile's %u interfaces", found, profile->interface_count);
		ok = false;
	}
	return ok;
}

bool ubus_configure(struct user_bus *b, struct ozzy_engine *engine)
{
	const struct ozzy_device_profile *profile = &engine->profile;
	const struct ozzy_usb_endpoint *bound[OZZY_PIPE_ROLES];
	struct ozzy_usb_endpoint eps[32];
	IOUSBConfigurationDescriptorPtr cd = NULL;
	unsigned int n, role, i;
	uint32_t bounce = 0;
	UInt8 config = 0;
	IOReturn kr;

	b->engine = engine;
	if (!b->io_running)
		return false;
	/* Only an unconfigured device gets a configuration: setting it again would tear down the
	 * interfaces under a driver that holds them. A failed GetConfiguration reads as 0. */
	(*b->dev)->GetConfiguration(b->dev, &config);
	if (config == 0) {
		kr = (*b->dev)->GetConfigurationDescriptorPtr(b->dev, 0, &cd);
		if (kr == kIOReturnSuccess)
			kr = (*b->dev)->SetConfiguration(b->dev, cd->bConfigurationValue);
		if (kr != kIOReturnSuccess) {
			ozzy_log("set configuration failed: 0x%x", (unsigned int)kr);
			return false;
		}
		usleep(500000);   /* as the kext after setConfiguration */
	}

	if (!open_interfaces(b, profile))
		return false;

	n = load_pipes(b, eps, sizeof(eps) / sizeof(eps[0]));
	ozzy_engine_bind_endpoints(engine, eps, n, bound);

	/* A bounce buffer per request, as large as an isochronous transfer of the bound pipes */
	for (role = 0; role < OZZY_PIPE_ROLES; role++) {
		uint32_t bytes;

		if (!bound[role] || (bound[role]->attributes & 3) != kUSBIsoc)
			continue;
		bytes = (uint32_t)ozzy_usb_endpoint_bytes(bound[role]) * OZZY_MAX_ISOC_PACKETS;
		if (bytes > bounce)
			bounce = bytes;
	}
	b->bounce_size = (bounce + 63) & ~63u;
	b->requests = calloc(UBUS_REQUESTS, sizeof(*b->requests));
	b->bounce_mem = b->bounce_size ? calloc(UBUS_REQUESTS, b->bounce_size) : NULL;
	if (!b->requests || (b->bounce_size && !b->bounce_mem))
		return false;
	for (i = 0; i < UBUS_REQUESTS; i++) {
		b->requests[i].bus = b;
		b->requests[i].bounce = b->bounce_mem ? b->bounce_mem + (size_t)i * b->bounce_size : NULL;
		b->requests[i].next_free = i + 1 < UBUS_REQUESTS ? &b->requests[i + 1] : NULL;
	}
	b->free_requests = &b->requests[0];
	return true;
}

void ubus_watch_reset(struct user_bus *b)
{
	uint64_t now = mach_absolute_time();
	int i;

	for (i = 0; i < UBUS_PIPE_SLOTS; i++) {
		__atomic_store_n(&b->pipes[i].last_done, now, __ATOMIC_RELAXED);
		__atomic_store_n(&b->pipes[i].last_data, now, __ATOMIC_RELAXED);
		__atomic_store_n(&b->pipes[i].error_streak, 0u, __ATOMIC_RELAXED);
		__atomic_store_n(&b->pipes[i].late, 0u, __ATOMIC_RELAXED);
		__atomic_store_n(&b->pipes[i].submitted, false, __ATOMIC_RELAXED);
	}
}

bool ubus_pipe_stalled(struct user_bus *b, uint8_t address, uint64_t now, uint64_t ticks)
{
	struct ubus_pipe *p = find_pipe(b, address);

	return p && __atomic_load_n(&p->submitted, __ATOMIC_RELAXED) &&
	       now - __atomic_load_n(&p->last_done, __ATOMIC_RELAXED) > ticks;
}

bool ubus_pipe_no_data(struct user_bus *b, uint8_t address, uint64_t now, uint64_t ticks)
{
	struct ubus_pipe *p = find_pipe(b, address);

	return p && __atomic_load_n(&p->submitted, __ATOMIC_RELAXED) &&
	       now - __atomic_load_n(&p->last_data, __ATOMIC_RELAXED) > ticks;
}

uint32_t ubus_pipe_errors(struct user_bus *b, uint8_t address)
{
	struct ubus_pipe *p = find_pipe(b, address);

	return p ? __atomic_load_n(&p->error_streak, __ATOMIC_RELAXED) : 0;
}

uint32_t ubus_pipe_late(struct user_bus *b, uint8_t address)
{
	struct ubus_pipe *p = find_pipe(b, address);

	return p ? __atomic_load_n(&p->late, __ATOMIC_RELAXED) : 0;
}

uint32_t ubus_pipe_refused(struct user_bus *b, uint8_t address)
{
	struct ubus_pipe *p = find_pipe(b, address);

	return p ? __atomic_load_n(&p->refused, __ATOMIC_RELAXED) : 0;
}

bool ubus_close(struct user_bus *b)
{
	unsigned int i;
	int pending;

	ubus_stop_timer(b);
	if (b->dev && b->intf_count)
		ubus_abort(b);
	for (i = 0; i < UBUS_MAX_INTERFACES; i++) {
		struct ubus_interface *intf = &b->intf[i];

		if (intf->source) {
			if (b->io_loop)
				CFRunLoopRemoveSource(b->io_loop, intf->source, kCFRunLoopDefaultMode);
			CFRelease(intf->source);
			intf->source = NULL;
		}
		if (intf->intf) {
			(*intf->intf)->USBInterfaceClose(intf->intf);
			(*intf->intf)->Release(intf->intf);
			intf->intf = NULL;
		}
	}
	b->intf_count = 0;
	stop_io(b);
	if (b->dev) {
		if (b->dev_open)
			(*b->dev)->USBDeviceClose(b->dev);
		(*b->dev)->Release(b->dev);
		b->dev = NULL;
		b->dev_open = false;
	}
	if (b->service) {
		IOObjectRelease(b->service);
		b->service = IO_OBJECT_NULL;
	}
	/* With the I/O thread gone no completion runs, so this is final. A transfer IOKit still holds
	 * may yet write into its request and bounce buffer: leave them allocated rather than free
	 * memory under it */
	pending = __atomic_load_n(&b->pending, __ATOMIC_ACQUIRE);
	if (pending > 0) {
		ozzy_log("%d transfers still outstanding at close: leaving their requests and buffers allocated", pending);
	} else {
		free(b->requests);
		free(b->bounce_mem);
	}
	b->requests = NULL;
	b->bounce_mem = NULL;
	b->free_requests = NULL;
	return pending <= 0;
}
