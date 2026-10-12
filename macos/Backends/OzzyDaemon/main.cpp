/* SPDX-License-Identifier: MIT */
/*
 * OzzyDaemon: SIP-free userspace driver for the Dynacord CMS 600-3. Waits
 * for the mixer to appear, runs one Session while it is attached, and
 * shares audio with OzzyHAL through /ozzy_shared_mem. Runs as a LaunchAgent.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <dispatch/dispatch.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <memory>
#include "../../../common/devices/ploytec/ploytec_defs.h"
#include "../../Shared/OzzyLog.h"
#include "Session.h"

static std::unique_ptr<Session> gSession;
static bool gVerbose;

#ifndef OZZY_VERSION
#define OZZY_VERSION "0.0.0-dev"
#endif

/* Point stderr at ~/Library/Logs/OzzyDaemon.log. launchd sets HOME for
 * agents but cannot expand it in the plist, so the daemon resolves it. */
static void RedirectStderrToUserLog()
{
	const char* home = getenv("HOME");
	if (!home || !*home)
		return;
	char dir[1024], path[1100];
	snprintf(dir, sizeof(dir), "%s/Library/Logs", home);
	if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
		snprintf(path, sizeof(path), "%s/Library", home);
		mkdir(path, 0700);
		mkdir(dir, 0755);
	}
	snprintf(path, sizeof(path), "%s/OzzyDaemon.log", dir);
	if (freopen(path, "a", stderr))
		setvbuf(stderr, nullptr, _IOLBF, 0);
}

static CFMutableDictionaryRef Matching()
{
	CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostDevice");
	int vid = DYNACORD_VENDOR_ID, pid = DYNACORD_PID_CMS600_3;
	CFNumberRef v = CFNumberCreate(nullptr, kCFNumberIntType, &vid), p = CFNumberCreate(nullptr, kCFNumberIntType, &pid);
	CFDictionarySetValue(m, CFSTR(kUSBVendorID), v);
	CFDictionarySetValue(m, CFSTR(kUSBProductID), p);
	CFRelease(v);
	CFRelease(p);
	return m;
}

static void OnSessionFinished()
{
	if (gSession && gSession->Finished()) {
		LogOzzyDaemonError("giving up on the CMS 600-3 until it is replugged");
		gSession.reset();
	}
}

static void Added(void*, io_iterator_t it)
{
	io_service_t s;
	while ((s = IOIteratorNext(it)) != IO_OBJECT_NULL) {
		if (gSession && gSession->Finished())
			gSession.reset();
		if (!gSession) {
			LogOzzyDaemon("CMS 600-3 attached");
			gSession.reset(new Session(s, gVerbose, OnSessionFinished));
			if (!gSession->Start())
				gSession.reset();
		}
		IOObjectRelease(s);
	}
}

static void Removed(void*, io_iterator_t it)
{
	io_service_t s;
	while ((s = IOIteratorNext(it)) != IO_OBJECT_NULL) {
		LogOzzyDaemon("CMS 600-3 detached");
		if (gSession && gSession->Is(s))
			gSession.reset();
		IOObjectRelease(s);
	}
}

int main(int argc, char** argv)
{
	bool logFile = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--version") == 0) {
			printf("%s\n", OZZY_VERSION);
			return 0;
		} else if (strcmp(argv[i], "--verbose") == 0) {
			gVerbose = true;
		} else if (strcmp(argv[i], "--log-file") == 0) {
			logFile = true;
		} else {
			fprintf(stderr, "usage: OzzyDaemon [--verbose] [--log-file] [--version]\n");
			return 2;
		}
	}
	if (logFile)
		RedirectStderrToUserLog();
	fprintf(stderr, "OzzyDaemon %s starting\n", OZZY_VERSION);
	LogOzzyDaemon("version %{public}s", OZZY_VERSION);
	signal(SIGTERM, SIG_IGN);
	signal(SIGINT, SIG_IGN);
	dispatch_source_t term = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
	dispatch_source_t intr = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0, dispatch_get_main_queue());
	dispatch_block_t quit = ^{
		gSession.reset();
		exit(0);
	};
	dispatch_source_set_event_handler(term, quit);
	dispatch_source_set_event_handler(intr, quit);
	dispatch_resume(term);
	dispatch_resume(intr);

	IONotificationPortRef port = IONotificationPortCreate(kIOMainPortDefault);
	CFRunLoopAddSource(CFRunLoopGetMain(), IONotificationPortGetRunLoopSource(port), kCFRunLoopDefaultMode);
	io_iterator_t added = IO_OBJECT_NULL, removed = IO_OBJECT_NULL;
	IOServiceAddMatchingNotification(port, kIOFirstMatchNotification, Matching(), Added, nullptr, &added);
	IOServiceAddMatchingNotification(port, kIOTerminatedNotification, Matching(), Removed, nullptr, &removed);
	Added(nullptr, added);
	Removed(nullptr, removed);
	LogOzzyDaemon("waiting for the CMS 600-3");
	CFRunLoopRun();
	return 0;
}
