/* SPDX-License-Identifier: MIT */
#include "../Backends/OzzyKext/KextStatus.h"
#include "test.h"
#include "tests.h"

void test_kext_status_underrun_is_success()
{
	CHECK_EQ(KextTransferStatus(0x81, kIOReturnUnderrun), 0);
	CHECK_EQ(KextTransferStatus(0x86, kIOReturnUnderrun), 0);
	CHECK_EQ(KextTransferStatus(0x81, kIOReturnSuccess), 0);
	CHECK_EQ(KextTransferStatus(0x02, kIOReturnUnderrun), (int)kIOReturnUnderrun);
	CHECK_EQ(KextTransferStatus(0x81, kIOReturnAborted), (int)kIOReturnAborted);
}
