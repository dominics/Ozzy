/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>

/*
 * Give the calling thread a Mach time-constraint policy sized for one wake
 * per 3 ms USB transfer: period 3 ms, computation 300 us, constraint 1 ms.
 * Every thread a USB completion passes through needs this on macOS, or
 * scheduling gaps of 10-30 ms starve the mixer's buffer (trunk cfb66c6).
 */
bool SetRealTimePolicy(char* err, size_t errlen);
