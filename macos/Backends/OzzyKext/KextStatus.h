/* SPDX-License-Identifier: MIT */
#pragma once
#include <IOKit/IOReturn.h>
#include <stdint.h>

/*
 * The status an engine sees for a finished transfer or iso interval. A short
 * IN transfer (the 3-byte feedback in its 64-byte slot, a short bulk read)
 * completes as kIOReturnUnderrun; for the engine that is data, not an error.
 */
static inline int KextTransferStatus(uint8_t pipeID, IOReturn status)
{
	if ((pipeID & 0x80) && status == kIOReturnUnderrun)
		return 0;
	return (int)status;
}
