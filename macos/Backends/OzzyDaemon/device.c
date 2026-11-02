/* SPDX-License-Identifier: MIT */
/* One driven device (device.h), as OzzyKext */

#include "device.h"
#include <mach/mach_time.h>
#include <stdlib.h>
#include "../../../common/ozzy_version.h"
#include "../../../engine/shared/ozzy_log.h"

#define POLL_MS          5      /* the mailbox */
#define WATCH_MS         250    /* the watchdog's period, as the kext's */
#define WATCH_DEAD_MS    500    /* a PCM pipe with no completion this long has stalled */
#define WATCH_RECOVER_S  10     /* between recoveries: not forever on a dead device */
#define FEEDBACK_DEAD_MS 1000   /* feedback without data this long: the device stopped streaming */
#define ERROR_STREAK     200    /* failed PCM out completions in a row: likewise */
#define SEGMENT_STALE_MS 1000

/* A registry string of the device ("USB Product Name"...) into @dst */
static void registry_string(io_service_t service, const char *key, volatile char *dst, size_t size)
{
	CFStringRef name = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	CFTypeRef value = IORegistryEntryCreateCFProperty(service, name, NULL, 0);
	char buf[64] = "";
	size_t i;

	CFRelease(name);
	if (value) {
		if (CFGetTypeID(value) == CFStringGetTypeID())
			CFStringGetCString(value, buf, sizeof(buf), kCFStringEncodingUTF8);
		CFRelease(value);
	}
	for (i = 0; i + 1 < size && buf[i]; i++)
		dst[i] = buf[i];
	dst[i] = 0;
}

static uint32_t registry_u32(io_service_t service, CFStringRef key)
{
	CFTypeRef value = IORegistryEntryCreateCFProperty(service, key, NULL, 0);
	uint32_t n = 0;

	if (value) {
		if (CFGetTypeID(value) == CFNumberGetTypeID())
			CFNumberGetValue(value, kCFNumberSInt32Type, &n);
		CFRelease(value);
	}
	return n;
}

/* --- Settings and rate (the mailbox) --- */

/* Restart with the settings the engine holds now; on failure the HAL drops the device */
static IOReturn restart_locked(struct daemon_device *d)
{
	if (ozzy_engine_restart(d->engine)) {
		ubus_watch_reset(&d->bus);
		return kIOReturnSuccess;
	}
	d->running = false;
	return kIOReturnIOError;
}

/* OZZY_METHOD_APPLY_SETTING, as OzzyKext::ApplySettingLocked */
static IOReturn apply_setting_locked(struct daemon_device *d, uint32_t id, uint32_t value)
{
	if (!d->running || d->sleeping)
		return kIOReturnNotReady;
	if (id > 0xFFFF || value > 0xFF)
		return kIOReturnBadArgument;
	if (id >= OZZY_NUM_SETTINGS || ozzy_setting_descs[id].kind != OZZY_SETTING_DEVICE ||
	    ozzy_engine_change_setting(d->engine, id, value) == OZZY_SETTING_INVALID)
		return kIOReturnBadArgument;
	return restart_locked(d);
}

/* OZZY_METHOD_SET_RATE, as OzzyKext::SetRate */
static IOReturn set_rate_locked(struct daemon_device *d, uint32_t rate)
{
	enum ozzy_setting_change change;

	if (!d->running || d->sleeping)
		return kIOReturnNotReady;
	change = ozzy_engine_change_rate(d->engine, rate);
	if (change == OZZY_SETTING_INVALID)
		return kIOReturnBadArgument;
	return change == OZZY_SETTING_RESTART ? restart_locked(d) : kIOReturnSuccess;
}

static void poll_fired(CFRunLoopTimerRef timer, void *info)
{
	struct daemon_device *d = info;
	uint32_t seq, method, args[2];

	(void)timer;
	if (!ozzy_daemon_take_request(d->seg.trailer, &seq, &method, args)) {
		/* MIDI out while the engine's timer isn't running (it checks the flag itself) */
		if (!d->bus.timer_thread_live && ozzy_daemon_take_midi_out_pending(d->seg.trailer)) {
			pthread_mutex_lock(&d->lock);
			if (d->running && !d->sleeping)
				ozzy_engine_midi_out_pending(d->engine);
			pthread_mutex_unlock(&d->lock);
		}
		return;
	}
	pthread_mutex_lock(&d->lock);
	IOReturn status;
	switch (method) {
	case OZZY_METHOD_APPLY_SETTING:
		status = apply_setting_locked(d, args[0], args[1]);
		break;
	case OZZY_METHOD_SET_RATE:
		status = set_rate_locked(d, args[0]);
		break;
	default:
		status = kIOReturnUnsupported;
		break;
	}
	pthread_mutex_unlock(&d->lock);
	if (status != kIOReturnSuccess)
		ozzy_log("request %u (%u, %u) failed: 0x%x", method, args[0], args[1], (unsigned int)status);
	/* After the restart has rewritten the shared memory: the HAL reads it on return */
	ozzy_daemon_ack(d->seg.trailer, seq, status);
}

/* --- Watchdog --- */

/*
 * As OzzyKext::WatchLocked: a PCM pipe that stopped completing gets the device a reselect and a
 * new start, counted in the stats. Also a device that keeps completing but stopped streaming:
 * its feedback without data, or PCM out failing in a row.
 */
static void watch_locked(struct daemon_device *d)
{
	static const enum ozzy_pipe_role roles[] = { OZZY_PIPE_PCM_OUT, OZZY_PIPE_PCM_IN };
	uint64_t now = mach_absolute_time();
	const char *why = NULL;
	uint8_t pipe = 0, fb, out;
	unsigned int i;
	bool streaming;

	if (!d->running || d->sleeping)
		return;
	for (i = 0; i < sizeof(roles) / sizeof(roles[0]) && !why; i++) {
		pipe = d->engine->pipe[roles[i]].address;
		if (pipe && ubus_pipe_stalled(&d->bus, pipe, now, ubus_ms_to_ticks(&d->bus, WATCH_DEAD_MS)))
			why = "no completion";
	}
	fb = d->engine->pipe[OZZY_PIPE_FEEDBACK].address;
	out = d->engine->pipe[OZZY_PIPE_PCM_OUT].address;
	if (!why && fb && ubus_pipe_no_data(&d->bus, fb, now, ubus_ms_to_ticks(&d->bus, FEEDBACK_DEAD_MS))) {
		pipe = fb;
		why = "no feedback";
	}
	if (!why && out && ubus_pipe_errors(&d->bus, out) >= ERROR_STREAK) {
		pipe = out;
		why = "failing";
	}
	if (!why || (d->recovered_at && now - d->recovered_at < ubus_ms_to_ticks(&d->bus, WATCH_RECOVER_S * 1000ull)))
		return;

	d->recovered_at = now;
	ozzy_log("pipe 0x%02x: %s, recovering the device", pipe, why);
	ozzy_engine_suspend(d->engine);
	streaming = ozzy_engine_start(d->engine);   /* a restart: it selects the interfaces again */
	ozzy_engine_stats_recovery(d->engine, streaming);
	if (streaming) {
		ubus_watch_reset(&d->bus);
		ozzy_log("recovered: streaming");
	} else {
		ozzy_log("recovered: start failed");
		ozzy_engine_stop(d->engine);
		d->running = false;
	}
}

static void watch_fired(CFRunLoopTimerRef timer, void *info)
{
	struct daemon_device *d = info;

	(void)timer;
	segment_tick(&d->seg);
	pthread_mutex_lock(&d->lock);
	watch_locked(d);
	pthread_mutex_unlock(&d->lock);
}

static CFRunLoopTimerRef add_timer(struct daemon_device *d, unsigned int ms, CFRunLoopTimerCallBack fn)
{
	CFRunLoopTimerContext ctx = { 0, d, NULL, NULL, NULL };
	CFTimeInterval interval = ms / 1000.0;
	CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + interval, interval, 0, 0, fn, &ctx);

	CFRunLoopAddTimer(CFRunLoopGetCurrent(), t, kCFRunLoopDefaultMode);
	return t;
}

static void remove_timer(CFRunLoopTimerRef *t)
{
	if (*t) {
		CFRunLoopTimerInvalidate(*t);
		CFRelease(*t);
		*t = NULL;
	}
}

/* --- Lifecycle --- */

static void free_device(struct daemon_device *d)
{
	remove_timer(&d->poll);
	remove_timer(&d->watch);
	ubus_close(&d->bus);
	segment_destroy(&d->seg);
	free(d->engine_mem);
	if (d->service)
		IOObjectRelease(d->service);
	pthread_mutex_destroy(&d->lock);
	free(d);
}

struct daemon_device *device_start(io_service_t service, const struct ozzy_device *model)
{
	struct daemon_device *d = calloc(1, sizeof(*d));
	char name[OZZY_DAEMON_SEGMENT_NAME_MAX];
	enum segment_result made;
	size_t size;
	IOReturn kr;

	if (!d)
		return NULL;
	pthread_mutex_init(&d->lock, NULL);
	IOObjectRetain(service);
	d->service = service;
	d->model = model;
	d->seg.fd = -1;
	d->location_id = registry_u32(service, CFSTR(kUSBDevicePropertyLocationID));
	ozzy_log("%s at 0x%08x (build %s)", model->name, d->location_id, OZZY_GIT_VERSION);

	kr = ubus_open(&d->bus, service);
	if (kr != kIOReturnSuccess) {
		ozzy_log("  open failed: 0x%x%s", (unsigned int)kr,
			 kr == kIOReturnExclusiveAccess ? " (another driver has it: the Ozzy kext?), leaving it" : "");
		goto fail;
	}
	if (model->family != OZZY_FAMILY_USB_MIDI && ubus_class_compliant(&d->bus)) {
		ozzy_log("  class compliant (interface 0 is Audio class): leaving it to the class driver");
		goto fail;
	}

	size = ozzy_engine_size(model);
	d->engine_mem = size ? calloc(1, size) : NULL;
	d->engine = d->engine_mem ? ozzy_engine_init(d->engine_mem, model) : NULL;
	if (!d->engine) {
		ozzy_log("  no engine for this device");
		goto fail;
	}

	ozzy_daemon_segment_name(name, d->location_id);
	made = segment_create(&d->seg, name, SEGMENT_STALE_MS);
	if (made != SEGMENT_CREATED && made != SEGMENT_REPLACED_STALE) {
		ozzy_log("  segment %s: %s", name, d->seg.reason);
		goto fail;
	}
	if (made == SEGMENT_REPLACED_STALE)
		ozzy_log("  segment %s: %s", name, d->seg.reason);
	/* The device's own strings, before attach (it keeps them over the model's name) */
	registry_string(service, kUSBVendorString, d->seg.shm->manufacturer_name, sizeof(d->seg.shm->manufacturer_name));
	registry_string(service, kUSBProductString, d->seg.shm->product_name, sizeof(d->seg.shm->product_name));
	registry_string(service, kUSBSerialNumberString, d->seg.shm->serial_number, sizeof(d->seg.shm->serial_number));

	if (!ubus_start_io(&d->bus) || !ubus_configure(&d->bus, d->engine)) {
		ozzy_log("  configuration failed");
		goto fail;
	}
	d->bus.trailer = d->seg.trailer;

	ozzy_engine_attach(d->engine, &d->bus.bus, d->seg.shm);
	ozzy_engine_set_usb_speed(d->engine, ubus_speed(&d->bus));
	ozzy_engine_set_driver_version(d->engine, OZZY_GIT_VERSION);
	ozzy_engine_set_anchors(d->engine, true);   /* OzzyHAL's zero timestamps */
	if (!ozzy_engine_start(d->engine)) {
		ozzy_log("  engine start failed");
		ozzy_engine_stop(d->engine);
		goto fail;
	}
	ubus_watch_reset(&d->bus);
	d->running = true;
	d->poll = add_timer(d, POLL_MS, poll_fired);
	d->watch = add_timer(d, WATCH_MS, watch_fired);
	ozzy_log("  driver ready (segment %s)", name);
	return d;

fail:
	free_device(d);
	return NULL;
}

void device_stop(struct daemon_device *d)
{
	remove_timer(&d->poll);
	remove_timer(&d->watch);
	/* Wait for a request in progress (its control requests fail fast once the device is gone) */
	pthread_mutex_lock(&d->lock);
	d->running = false;
	pthread_mutex_unlock(&d->lock);
	if (d->seg.shm) {
		d->seg.shm->audio.hardware_present = false;
		d->seg.shm->audio.driver_ready = false;
	}
	if (d->engine)
		ozzy_engine_stop(d->engine);   /* aborts what's in flight */
	ozzy_log("%s at 0x%08x stopped", d->model->name, d->location_id);
	free_device(d);
}

void device_sleep(struct daemon_device *d)
{
	pthread_mutex_lock(&d->lock);
	if (d->running && !d->sleeping) {
		ozzy_log("sleep: suspending");
		ozzy_engine_suspend(d->engine);
		d->sleeping = true;
	}
	pthread_mutex_unlock(&d->lock);
}

/* Transfers in flight across a sleep may never complete on some devices, so a new start (as
 * the kext's setPowerState) */
void device_wake(struct daemon_device *d)
{
	pthread_mutex_lock(&d->lock);
	if (d->sleeping) {
		d->sleeping = false;
		if (d->running) {
			ozzy_log("wake: starting again");
			if (ozzy_engine_start(d->engine)) {
				ubus_watch_reset(&d->bus);
			} else {
				ozzy_log("start after wake failed");
				ozzy_engine_stop(d->engine);
				d->running = false;
			}
		}
	}
	pthread_mutex_unlock(&d->lock);
}
