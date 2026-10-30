/* SPDX-License-Identifier: MIT */
#include <cstring>
#include "../OzzyHAL/OzzyDevices.h"
#include "test.h"
#include "tests.h"

/* A Xone publishes only the main device, the CMS 600-3 adds MON and AUX, and
 * nothing is published while disconnected. */
void test_devices_count()
{
	CHECK_EQ(OzzyDeviceCount(false, false), 0);
	CHECK_EQ(OzzyDeviceCount(false, true), 0);
	CHECK_EQ(OzzyDeviceCount(true, false), 1);
	CHECK_EQ(OzzyDeviceCount(true, true), 3);
}

void test_devices_lookup()
{
	CHECK_EQ(OzzyDeviceIndex(kOzzyDeviceID, true, true), 0);
	CHECK_EQ(OzzyDeviceIndex(kOzzyInputStreamID, true, true), 0);
	CHECK_EQ(OzzyDeviceIndex(kOzzyOutputStreamID, true, true), 0);
	CHECK_EQ(OzzyDeviceIndex(kOzzyMonDeviceID, true, true), 1);
	CHECK_EQ(OzzyDeviceIndex(kOzzyMonStreamID, true, true), 1);
	CHECK_EQ(OzzyDeviceIndex(kOzzyAuxDeviceID, true, true), 2);
	CHECK_EQ(OzzyDeviceIndex(kOzzyAuxStreamID, true, true), 2);

	/* A Xone never answers for the mono devices, and nothing answers while disconnected. */
	CHECK_EQ(OzzyDeviceIndex(kOzzyDeviceID, true, false), 0);
	CHECK_EQ(OzzyDeviceIndex(kOzzyMonDeviceID, true, false), -1);
	CHECK_EQ(OzzyDeviceIndex(kOzzyAuxStreamID, true, false), -1);
	CHECK_EQ(OzzyDeviceIndex(kOzzyDeviceID, false, true), -1);

	/* Input-only devices have no output stream, so ID 0 matches nothing. */
	CHECK_EQ(OzzyDeviceIndex(0, true, true), -1);
	CHECK_EQ(OzzyDeviceIndex(kOzzyVolumeControlID, true, true), -1);

	CHECK(OzzyIsStream(kOzzyMonStreamID));
	CHECK(OzzyIsStream(kOzzyOutputStreamID));
	CHECK(!OzzyIsStream(kOzzyMonDeviceID));
	CHECK(!OzzyIsStream(0));
}

/* MON and AUX read USB inputs 4 and 3, have no output, and get distinct UIDs. */
void test_devices_mono_channels()
{
	const OzzyDeviceInfo& mon = kOzzyDevices[OzzyDeviceIndex(kOzzyMonDeviceID, true, true)];
	const OzzyDeviceInfo& aux = kOzzyDevices[OzzyDeviceIndex(kOzzyAuxDeviceID, true, true)];
	CHECK_EQ(mon.inputChannel, 4);
	CHECK_EQ(aux.inputChannel, 3);
	CHECK_EQ(mon.outputStream, 0);
	CHECK_EQ(aux.outputStream, 0);
	CHECK_EQ(kOzzyDevices[0].inputChannel, 0);
	CHECK(std::strcmp(mon.uidSuffix, aux.uidSuffix) != 0);
	CHECK(std::strcmp(mon.uidSuffix, "") != 0);
}
