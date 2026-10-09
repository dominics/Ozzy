/* SPDX-License-Identifier: MIT */
#include "RealTime.h"
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <stdio.h>

static uint32_t UsToAbs(unsigned us)
{
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	return (uint32_t)((unsigned long long)us * 1000 * tb.denom / tb.numer);
}

bool SetRealTimePolicy(char* err, size_t errlen)
{
	thread_time_constraint_policy_data_t p;
	p.period = UsToAbs(3000);
	p.computation = UsToAbs(300);
	p.constraint = UsToAbs(1000);
	p.preemptible = TRUE;
	kern_return_t kr = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
					     (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
	if (kr != KERN_SUCCESS) {
		snprintf(err, errlen, "thread_policy_set: %s", mach_error_string(kr));
		return false;
	}
	return true;
}
