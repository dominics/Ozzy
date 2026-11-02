#include <stdio.h>
#include <string.h>
#include "rt.h"

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>

/* Name libusb's darwin backend gives the thread that runs its CFRunLoop. */
#define LIBUSB_DARWIN_THREAD "org.libusb.device-hotplug"

static uint32_t us_to_abs(unsigned us)
{
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	return (uint32_t)((unsigned long long)us * 1000 * tb.denom / tb.numer);
}

/* Each thread wakes about once per 3 ms OUT transfer and needs well under
 * 300 us; it must get that within 1 ms of waking. */
static int set_policy(pthread_t th, const char *what, char *err, size_t errlen)
{
	thread_time_constraint_policy_data_t p = {
		.period = us_to_abs(3000),
		.computation = us_to_abs(300),
		.constraint = us_to_abs(1000),
		.preemptible = TRUE,
	};
	kern_return_t kr = thread_policy_set(pthread_mach_thread_np(th), THREAD_TIME_CONSTRAINT_POLICY,
					     (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
	if (kr != KERN_SUCCESS) {
		snprintf(err, errlen, "real-time policy for %s: %s", what, mach_error_string(kr));
		return -1;
	}
	return 0;
}

static int find_thread(const char *name, pthread_t *out)
{
	thread_act_array_t threads;
	mach_msg_type_number_t n;
	int found = -1;

	if (task_threads(mach_task_self(), &threads, &n) != KERN_SUCCESS)
		return -1;
	for (mach_msg_type_number_t i = 0; i < n; i++) {
		pthread_t th = pthread_from_mach_thread_np(threads[i]);
		char buf[64];
		if (found < 0 && th && pthread_getname_np(th, buf, sizeof(buf)) == 0 && strcmp(buf, name) == 0) {
			*out = th;
			found = 0;
		}
		mach_port_deallocate(mach_task_self(), threads[i]);
	}
	vm_deallocate(mach_task_self(), (vm_address_t)threads, n * sizeof(*threads));
	return found;
}

int rt_usb_threads(pthread_t event_thread, char *err, size_t errlen)
{
	pthread_t lib;
	if (find_thread(LIBUSB_DARWIN_THREAD, &lib) < 0) {
		snprintf(err, errlen, "libusb thread %s not found", LIBUSB_DARWIN_THREAD);
		return -1;
	}
	if (set_policy(lib, "libusb thread", err, errlen) < 0)
		return -1;
	return set_policy(event_thread, "event thread", err, errlen);
}

#else

int rt_usb_threads(pthread_t event_thread, char *err, size_t errlen)
{
	(void)event_thread, (void)err, (void)errlen;
	return 0;
}

#endif
