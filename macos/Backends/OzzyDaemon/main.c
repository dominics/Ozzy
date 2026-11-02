/* SPDX-License-Identifier: MIT */
/*
 * ozzy-daemon: drives the listed devices it is allowed to (the CMS 600-3 unless told otherwise)
 * from user space, one segment per device, for OzzyHAL and OzzyMIDI. A LaunchAgent in each GUI
 * session: only the console user's daemon drives a device, the others release it and its
 * segment (handover.h). A device someone else holds (another user's daemon, a kext) is retried
 * until it lets go. The main thread is the control thread: device matching and termination,
 * console changes, system sleep and wake, retries, and each device's mailbox and watchdog
 * timers.
 */

#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "device.h"
#include "../../../common/ozzy_devices.h"
#include "../../../common/ozzy_version.h"
#include "../../../engine/shared/ozzy_log.h"

#define DEFAULT_DEVICE "DynacordCMS600"
#define MAX_ALLOWED    16

static const char *allowed[MAX_ALLOWED] = { DEFAULT_DEVICE };
static unsigned int allowed_count = 1;
static bool allow_all;
static struct daemon_device *devices;
static io_connect_t power_port;
static uid_t console_uid = HANDOVER_NO_CONSOLE;

static bool is_allowed(const struct ozzy_device *model)
{
	unsigned int i;

	if (allow_all)
		return true;
	for (i = 0; i < allowed_count; i++) {
		if (strcmp(allowed[i], model->key) == 0)
			return true;
	}
	return false;
}

/* --- Handover: doing what handover.h decides --- */

static void apply(struct daemon_device *d, enum handover_action action);

static void retry_fired(CFRunLoopTimerRef timer, void *info)
{
	struct daemon_device *d = info;

	(void)timer;
	CFRelease(d->retry);   /* a one-shot timer: already invalidated */
	d->retry = NULL;
	apply(d, handover_retry(&d->handover));
}

static void retry_after(struct daemon_device *d, unsigned int ms)
{
	CFRunLoopTimerContext ctx = { 0, d, NULL, NULL, NULL };

	if (d->retry) {
		CFRunLoopTimerInvalidate(d->retry);
		CFRelease(d->retry);
	}
	d->retry = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + ms / 1000.0, 0, 0, 0, retry_fired, &ctx);
	CFRunLoopAddTimer(CFRunLoopGetMain(), d->retry, kCFRunLoopDefaultMode);
}

static void apply(struct daemon_device *d, enum handover_action action)
{
	switch (action) {
	case HANDOVER_NONE:
		break;
	case HANDOVER_START:
		apply(d, handover_started(&d->handover, device_drive(d)));
		break;
	case HANDOVER_STOP:
		device_release(d);
		break;
	case HANDOVER_RETRY_SOON:
		retry_after(d, HANDOVER_SOON_MS);
		break;
	case HANDOVER_RETRY_SLOWLY:
		if (!d->slow_logged) {
			ozzy_log("%s at 0x%08x still held; trying again each %u s", d->model->name, d->location_id,
				 HANDOVER_SLOW_MS / 1000);
			d->slow_logged = true;
		}
		retry_after(d, HANDOVER_SLOW_MS);
		break;
	case HANDOVER_GIVE_UP:
		ozzy_log("%s at 0x%08x: not driving it until a replug or a user switch", d->model->name, d->location_id);
		break;
	}
}

static void log_console(void)
{
	if (console_uid == HANDOVER_NO_CONSOLE)
		ozzy_log("console: the login window");
	else
		ozzy_log("console: uid %u%s", (unsigned int)console_uid, console_uid == getuid() ? " (us)" : "");
}

/* The console user as the handover counts it: nobody at the login window */
static uid_t read_console_uid(SCDynamicStoreRef store)
{
	uid_t uid = 0;
	CFStringRef name = SCDynamicStoreCopyConsoleUser(store, &uid, NULL);
	bool login_window;

	if (!name)
		return HANDOVER_NO_CONSOLE;
	login_window = CFEqual(name, CFSTR("loginwindow"));
	CFRelease(name);
	return login_window ? HANDOVER_NO_CONSOLE : uid;
}

static void console_changed(SCDynamicStoreRef store, CFArrayRef keys, void *info)
{
	uid_t uid = read_console_uid(store);
	struct daemon_device *d;

	(void)keys;
	(void)info;
	if (uid == console_uid)
		return;
	console_uid = uid;
	log_console();
	for (d = devices; d; d = d->next) {
		d->busy_logged = false;
		d->slow_logged = false;
		apply(d, handover_console(&d->handover, uid));
	}
}

/* Follow the console user; false if the system won't say (then this daemon counts as in front) */
static bool watch_console(void)
{
	SCDynamicStoreRef store = SCDynamicStoreCreate(NULL, CFSTR("ozzy-daemon"), console_changed, NULL);
	CFStringRef key;
	CFArrayRef keys;
	CFRunLoopSourceRef source;

	if (!store)
		return false;
	key = SCDynamicStoreKeyCreateConsoleUser(NULL);
	keys = CFArrayCreate(NULL, (const void **)&key, 1, &kCFTypeArrayCallBacks);
	if (!SCDynamicStoreSetNotificationKeys(store, keys, NULL)) {
		CFRelease(keys);
		CFRelease(key);
		CFRelease(store);
		return false;
	}
	CFRelease(keys);
	CFRelease(key);
	source = SCDynamicStoreCreateRunLoopSource(NULL, store, 0);
	CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopDefaultMode);
	CFRelease(source);
	console_uid = read_console_uid(store);
	return true;
}

/* --- Devices --- */

static void device_added(void *refcon, io_iterator_t it)
{
	io_service_t s;

	(void)refcon;
	while ((s = IOIteratorNext(it)) != IO_OBJECT_NULL) {
		CFNumberRef vid_ref = IORegistryEntryCreateCFProperty(s, CFSTR(kUSBVendorID), NULL, 0);
		CFNumberRef pid_ref = IORegistryEntryCreateCFProperty(s, CFSTR(kUSBProductID), NULL, 0);
		int vid = 0, pid = 0;
		const struct ozzy_device *model;

		if (vid_ref) {
			CFNumberGetValue(vid_ref, kCFNumberIntType, &vid);
			CFRelease(vid_ref);
		}
		if (pid_ref) {
			CFNumberGetValue(pid_ref, kCFNumberIntType, &pid);
			CFRelease(pid_ref);
		}
		model = ozzy_device_lookup((uint16_t)vid, (uint16_t)pid);
		if (model && is_allowed(model)) {
			struct daemon_device *d = device_new(s, model);

			if (d) {
				d->next = devices;
				devices = d;
				handover_init(&d->handover, getuid());
				handover_console(&d->handover, console_uid);
				if (!d->handover.front) {
					ozzy_log("%s at 0x%08x: another user is in front, leaving it to them", model->name,
						 d->location_id);
					device_clear_stale(d);
				}
				apply(d, handover_device(&d->handover, true));
			}
		}
		IOObjectRelease(s);
	}
}

static void device_removed(void *refcon, io_iterator_t it)
{
	io_service_t s;

	(void)refcon;
	while ((s = IOIteratorNext(it)) != IO_OBJECT_NULL) {
		struct daemon_device **link = &devices;

		while (*link) {
			struct daemon_device *d = *link;

			if (IOObjectIsEqualTo(d->service, s)) {
				*link = d->next;
				ozzy_log("%s at 0x%08x disconnected", d->model->name, d->location_id);
				apply(d, handover_device(&d->handover, false));
				device_free(d);
				break;
			}
			link = &d->next;
		}
		IOObjectRelease(s);
	}
}

static void power_changed(void *refcon, io_service_t service, natural_t type, void *argument)
{
	struct daemon_device *d;

	(void)refcon;
	(void)service;
	switch (type) {
	case kIOMessageCanSystemSleep:
		IOAllowPowerChange(power_port, (long)argument);
		break;
	case kIOMessageSystemWillSleep:
		for (d = devices; d; d = d->next)
			device_sleep(d);
		IOAllowPowerChange(power_port, (long)argument);
		break;
	case kIOMessageSystemHasPoweredOn:
		for (d = devices; d; d = d->next)
			device_wake(d);
		break;
	default:
		break;
	}
}

static void quit(void)
{
	while (devices) {
		struct daemon_device *d = devices;

		devices = d->next;
		device_free(d);
	}
	exit(0);
}

/* stderr (the engine's log) to ~/Library/Logs/OzzyDaemon.log */
static void log_to_file(void)
{
	const char *home = getenv("HOME");
	char dir[1024], path[1100];

	if (!home)
		return;
	snprintf(dir, sizeof(dir), "%s/Library/Logs", home);
	if (mkdir(dir, 0755) != 0 && errno != EEXIST)
		return;
	snprintf(path, sizeof(path), "%s/OzzyDaemon.log", dir);
	if (freopen(path, "a", stderr))
		setvbuf(stderr, NULL, _IOLBF, 0);
}

static void usage(void)
{
	fprintf(stderr, "usage: ozzy-daemon [--device KEY]... [--all] [--log-file] [--version] [--list]\n"
			"  --device KEY  drive this model too (a key from --list); the default is " DEFAULT_DEVICE "\n"
			"  --all         drive any listed model the kext doesn't hold\n");
}

int main(int argc, char **argv)
{
	IONotificationPortRef port, power_notify = NULL;
	io_iterator_t added = IO_OBJECT_NULL, removed = IO_OBJECT_NULL;
	io_object_t power_notifier = IO_OBJECT_NULL;
	dispatch_source_t term, intr;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--version") == 0) {
			printf("%s\n", OZZY_GIT_VERSION);
			return 0;
		} else if (strcmp(argv[i], "--list") == 0) {
			unsigned int k;

			for (k = 0; k < OZZY_NUM_DEVICES; k++)
				printf("%-20s %04x:%04x %s\n", ozzy_devices[k].key, ozzy_devices[k].vid, ozzy_devices[k].pid,
				       ozzy_devices[k].name);
			return 0;
		} else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc && allowed_count < MAX_ALLOWED) {
			allowed[allowed_count++] = argv[++i];
		} else if (strcmp(argv[i], "--all") == 0) {
			allow_all = true;
		} else if (strcmp(argv[i], "--log-file") == 0) {
			log_to_file();
		} else {
			usage();
			return 2;
		}
	}
	ozzy_log("ozzy-daemon %s starting", OZZY_GIT_VERSION);

	signal(SIGTERM, SIG_IGN);
	signal(SIGINT, SIG_IGN);
	term = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
	intr = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0, dispatch_get_main_queue());
	dispatch_source_set_event_handler(term, ^{ quit(); });
	dispatch_source_set_event_handler(intr, ^{ quit(); });
	dispatch_resume(term);
	dispatch_resume(intr);

	if (watch_console()) {
		log_console();
	} else {
		ozzy_log("no console user notifications: driving devices as if in front");
		console_uid = getuid();
	}

	power_port = IORegisterForSystemPower(NULL, &power_notify, power_changed, &power_notifier);
	if (power_port)
		CFRunLoopAddSource(CFRunLoopGetMain(), IONotificationPortGetRunLoopSource(power_notify),
				   kCFRunLoopDefaultMode);
	else
		ozzy_log("no sleep and wake notifications");

	port = IONotificationPortCreate(MACH_PORT_NULL);   /* the default main port, before macOS 12 too */
	CFRunLoopAddSource(CFRunLoopGetMain(), IONotificationPortGetRunLoopSource(port), kCFRunLoopDefaultMode);
	IOServiceAddMatchingNotification(port, kIOFirstMatchNotification, IOServiceMatching("IOUSBHostDevice"),
					 device_added, NULL, &added);
	IOServiceAddMatchingNotification(port, kIOTerminatedNotification, IOServiceMatching("IOUSBHostDevice"),
					 device_removed, NULL, &removed);
	device_added(NULL, added);
	device_removed(NULL, removed);

	CFRunLoopRun();
	return 0;
}
