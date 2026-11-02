#ifndef PLOYTEC_PLAY_RT_H
#define PLOYTEC_PLAY_RT_H

#include <pthread.h>
#include <stddef.h>

/*
 * Put the threads that USB completions pass through under a Mach
 * time-constraint (real-time) policy: libusb's internal macOS thread, which
 * receives completions from IOKit, and event_thread, which runs our
 * callbacks. Ordinary threads can go unscheduled for 10-30 ms at a time, longer
 * than a short iso OUT queue lasts. Does nothing on other systems. Returns 0,
 * or -1 with a message in err.
 */
int rt_usb_threads(pthread_t event_thread, char *err, size_t errlen);

#endif
