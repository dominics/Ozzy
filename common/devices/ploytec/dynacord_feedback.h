/* SPDX-License-Identifier: MIT */
#ifndef DYNACORD_FEEDBACK_H
#define DYNACORD_FEEDBACK_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/*
 * Parse one iso packet from the feedback endpoint. Each packet is a sliding
 * window of the frames the device consumed in each of the last 3 ms, newest
 * first. Returns the newest count, or -1 if the packet is empty.
 */
int feedback_parse(const uint8_t *pkt, int len);

#endif
